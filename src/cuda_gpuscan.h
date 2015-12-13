/*
 * cuda_gpuscan.h
 *
 * CUDA device code specific to GpuScan logic
 * --
 * Copyright 2011-2015 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2015 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef CUDA_GPUSCAN_H
#define CUDA_GPUSCAN_H

/*
 * Sequential Scan using GPU/MIC acceleration
 *
 * It packs a kern_parambuf and kern_resultbuf structure within a continuous
 * memory ares, to transfer (usually) small chunk by one DMA call.
 *
 * +----------------+       -----
 * | kern_parambuf  |         ^
 * | +--------------+         |
 * | | length   o--------------------+
 * | +--------------+         |      | kern_vrelation is located just after
 * | | nparams      |         |      | the kern_parambuf (because of DMA
 * | +--------------+         |      | optimization), so head address of
 * | | poffset[0]   |         |      | kern_gpuscan + parambuf.length
 * | | poffset[1]   |         |      | points kern_resultbuf.
 * | |    :         |         |      |
 * | | poffset[M-1] |         |      |
 * | +--------------+         |      |
 * | | variable     |         |      |
 * | | length field |         |      |
 * | | for Param /  |         |      |
 * | | Const values |         |      |
 * | |     :        |         |      |
 * +-+--------------+  -----  |  <---+
 * | kern_resultbuf |    ^    |
 * | +--------------+    |    |  Area to be sent to OpenCL device.
 * | | nrels (=1)   |    |    |  Forward DMA shall be issued here.
 * | +--------------+    |    |
 * | | nitems       |    |    |
 * | +--------------+    |    |
 * | | nrooms (=N)  |    |    |
 * | +--------------+    |    |
 * | | errcode      |    |    V
 * | +--------------+    |  -----
 * | | rindex[0]    |    |
 * | | rindex[1]    |    |  Area to be written back from OpenCL device.
 * | |     :        |    |  Reverse DMA shall be issued here.
 * | | rindex[N-1]  |    V
 * +-+--------------+  -----
 *
 * Gpuscan kernel code assumes all the fields shall be initialized to zero.
 */
typedef struct {
	kern_parambuf	kparams;
} kern_gpuscan;

#define KERN_GPUSCAN_PARAMBUF(kgpuscan)			\
	((kern_parambuf *)(&(kgpuscan)->kparams))
#define KERN_GPUSCAN_PARAMBUF_LENGTH(kgpuscan)	\
	STROMALIGN((kgpuscan)->kparams.length)
#define KERN_GPUSCAN_RESULTBUF(kgpuscan)		\
	((kern_resultbuf *)((char *)&(kgpuscan)->kparams +				\
						STROMALIGN((kgpuscan)->kparams.length)))
#define KERN_GPUSCAN_RESULTBUF_LENGTH(kgpuscan)	\
	STROMALIGN(offsetof(kern_resultbuf,			\
		results[KERN_GPUSCAN_RESULTBUF(kgpuscan)->nrels * \
				KERN_GPUSCAN_RESULTBUF(kgpuscan)->nrooms]))
#define KERN_GPUSCAN_LENGTH(kgpuscan)			\
	(offsetof(kern_gpuscan, kparams) +			\
	 KERN_GPUSCAN_PARAMBUF_LENGTH(kgpuscan) +	\
	 KERN_GPUSCAN_RESULTBUF_LENGTH(kgpuscan))
#define KERN_GPUSCAN_DMASEND_OFFSET(kgpuscan)	0
#define KERN_GPUSCAN_DMASEND_LENGTH(kgpuscan)	\
	(KERN_GPUSCAN_PARAMBUF_LENGTH(kgpuscan) +	\
	 offsetof(kern_resultbuf, results[0]))
#define KERN_GPUSCAN_DMARECV_OFFSET(kgpuscan)	\
	KERN_GPUSCAN_PARAMBUF_LENGTH(kgpuscan)
#define KERN_GPUSCAN_DMARECV_LENGTH(kgpuscan)	\
	KERN_GPUSCAN_RESULTBUF_LENGTH(kgpuscan)

#ifdef __CUDACC__
/*
 * gpuscan_writeback_results
 *
 * It writes back the calculation result of gpuscan.
 */
STATIC_FUNCTION(void)
gpuscan_writeback_results(kern_resultbuf *kresults, int result)
{
	__shared__ cl_uint base;
	size_t		result_index = get_global_id() + 1;
	cl_uint		binary;
	cl_uint		offset;
	cl_uint		nitems;

	assert(kresults->nrels == 1);

	/*
	 * A typical usecase of arithmetic_stairlike_add with binary value:
	 * It takes 1 if thread wants to return a status to the host side,
	 * then stairlike-add returns a relative offset within workgroup,
	 * and we can adjust this offset by global base index.
	 */
	binary = (result != 0 ? 1 : 0);
	offset = arithmetic_stairlike_add(binary, &nitems);
	if (get_local_id() == 0)
		base = atomicAdd(&kresults->nitems, nitems);
	__syncthreads();

	/*
	 * Write back the row-index that passed evaluation of the qualifier,
	 * or needs re-check on the host side. In case of re-check, row-index
	 * shall be a negative number.
	 */
	if (result > 0)
		kresults->results[base + offset] =  result_index;
	else if (result < 0)
		kresults->results[base + offset] = -result_index;
}

/*
 * forward declaration of the function to be generated on the fly
 */
STATIC_FUNCTION(cl_bool)
gpuscan_qual_eval(kern_context *kcxt,
				  kern_data_store *kds,
				  kern_data_store *ktoast,
				  size_t kds_index);

/*
 * gpuscan_projection_row
 *
 * It constructs a result tuple of GpuScan. If layout of the result tuple
 * is compatible to the base relation's definition, we just copy the source
 * tuple to the destination.
 * Elsewhere, we apply projection to generate the result.
 */
STATIC_FUNCTION(void)
gpuscan_projection_row(kern_gpuscan *kgpuscan,
					   kern_context *kcxt,
					   kern_colmeta *cmeta_src,
					   kern_tupitem	*tupitem_src,
					   kern_data_store *kds_dst)
{
	cl_uint				tuplen;
	cl_uint				required;
	cl_uint				offset;
	cl_uint				count;
	__shared__ cl_uint	prev_nitems;
	__shared__ cl_uint	prev_usage;
	cl_uint				item_index;
	cl_uint				htup_offset;

	/*
	 * step.1 - compute length of the result tuple.
	 *
	 * Unless GPUSCAN_DEVICE_PROJECTION is not defined, we have
	 * no particular projection on the device side, so just copy
	 * the source tuple-item to the destination buffer.
	 * If we have, GPUSCAN_DEVICE_PROJECTION shall be extracted
	 * to implement the following jobs.
	 *  1. declaration of 'dst_values' and 'dst_isnull' array
	 *     with appropriate length.
	 *  2. initialization of the above array.
	 *  3. and, computing the tuple_len
	 */
#ifdef GPUSCAN_DEVICE_PROJECTION
	Datum		tup_values[GPUSCAN_DEVICE_PROJECTION_NFIELDS];
	cl_bool		tup_isnull[GPUSCAN_DEVICE_PROJECTION_NFIELDS];
	cl_bool		tup_internal[GPUSCAN_DEVICE_PROJECTION_NFIELDS];

	GPUSCAN_DEVICE_PROJECTION(KDS_FORMAT_ROW);

	tuple_len = compute_heaptuple_size(kds,
									   tup_values,
									   tup_isnull,
									   tup_internal);
#else
	tuple_len = (tupitem_src != NULL ? tupitem_src->t_len : 0);
#endif
	assert(tupitem_src != NULL ? tuple_len > 0 : tuple_len == 0);

	/*
	 * step.2 - increment nitems of kds_dst
	 */
	offset = arithmetic_stairlike_add(tupitem_src != NULL ? 1 : 0, &count);
	if (get_local_id() == 0)
	{
		if (count > 0)
			nitems_prev = atomicAdd(&kds_dst->nitems, count);
		else
			nitems_prev = 0;
	}
	__syncthreads();

	if (nitems_prev + count > kds_dst->nrooms)
	{
		STROM_SET_ERROR(&kcxt->e, StromError_DataStoreNoSpace);
		return;
	}
	item_index = nitems_prev + offset;

	/*
	 * step.3 - increment buffer usage on kds_dst
	 */
	required = MAXALIGN(offsetof(kern_tupitem, htup) + tuple_len);
	offset = arithmetic_stairlike_add(required, &count);
	if (get_local_id() == 0)
	{
		if (count > 0)
			usage_prev = atomicAdd(&kds_dst->usage, count);
		else
			usage_prev = 0;
	}
	__syncthreads();

	if (KERN_DATA_STORE_HEAD_LENGTH(kds_dst) +
		STROMALIGN(sizeof(cl_uint) * kds_dst->nitems) +
		usage_prev + count > kds_dst->length)
	{
		STROM_SET_ERROR(&kcxt->e, StromError_DataStoreNoSpace);
		return;
	}
	htup_offset = kds_dst->length - (usage_prev + offset + required);
	assert(htup_offset == MAXALIGN(htup_offset));

	/*
	 * step.4 - construction of the result tuple
	 */
	if (tupsrc != NULL)
	{
		cl_uint		   *tupitem_idx
			= (cl_uint *)KERN_DATA_STORE_BODY(kds_dst);
		kern_tupitem   *tupitem_dst
			= (kern_tupitem *)((char *)kds_dst + htup_offset);

#ifdef GPUSCAN_DEVICE_PROJECTION
		form_kern_heaptuple(kds_dst, tupitem_dst,
							tup_values, tup_isnull, tup_internal);
#else
		memcpy(tupitem_dst, tupitem_src,
			   offsetof(kern_tupitem, htup) + tuplen);
#endif
		tupitem_idx[item_index] = htup_offset;
	}
}

STATIC_FUNCTION(void)
gpuscan_projection_slot(kern_gpuscan *kgpuscan,
						kern_context *kcxt,
						kern_data_store *kds_dst,
						kern_data_store *kds_src,
						kern_tupitem *tupitem_src)
{
	cl_uint				tuple_len;
	cl_uint				offset;
	cl_uint				count;
	cl_uint				dst_index;
	__shared__ cl_uint	base;

	/*
	 * Sanity checks
	 */
	assert(kds_dst->format == KDS_FORMAT_SLOT);

	/*
	 * step.1 - computing number of rows to be written
	 */
	offset = arithmetic_stairlike_add(tupitem_src != NULL ? 1 : 0, &count);
	if (get_local_id() == 0)
	{
		if (count > 0)
			base = atomicAdd(&kds_dst->nitems, count);
		else
			base = 0;
	}
	__syncthreads();

	if (base + count > kds_dst->nrooms)
		STROM_SET_ERROR(&kcxt->e, StromError_DataStoreNoSpace);
	else
	{
		/*
		 * NOTE: GPUSCAN_DEVICE_PROJECTION internally acquires variable-
		 * length buffer, thus involves reduction operations using shared
		 * memory and 
		 */
		cl_uint		dst_index = base + offset;
		Datum	   *tup_values = KERN_DATA_STORE_VALUES(kds_dst, dst_index);
		cl_bool	   *tup_isnull = KERN_DATA_STORE_ISNULL(kds_dst, dst_index);
#ifdef GPUSCAN_DEVICE_PROJECTION
		cl_bool		tup_internal[GPUSCAN_DEVICE_PROJECTION_NFIELDS];
		cl_uint		i, ncols = kds_dst->ncols;
		cl_uint		vl_buflen = 0;

		GPUSCAN_DEVICE_PROJECTION(KDS_FORMAT_SLOT);
#else
		if (tupitem_src != NULL)
			deform_kern_heaptuple(kds_src,
								  tupitem_src,
								  kds_dst->ncols,
								  tup_values,
								  tup_isnull);
		}
#endif
	}
}

/*
 * kernel entrypoint of gpuscan
 */
KERNEL_FUNCTION(void)
gpuscan_qual(kern_gpuscan *kgpuscan,	/* in/out */
			 kern_data_store *kds_src,	/* in */
			 kern_data_store *kds_dst)	/* out */
{
	kern_parambuf  *kparams = KERN_GPUSCAN_PARAMBUF(kgpuscan);
	kern_resultbuf *kresults = KERN_GPUSCAN_RESULTBUF(kgpuscan);
	kern_context	kcxt;
	kern_tupitem   *tupitem;
	size_t			kds_index = get_global_id();
	cl_int			rc = 0;

	INIT_KERNEL_CONTEXT(&kcxt,gpuscan_qual,kparams);

	if (kds_index < kds->nitems)
	{
		tupitem = KERN_DATA_STORE_TUPITEM(kds_src, kds_index);
		if (!gpuscan_qual_eval(&kcxt, tupitem))
			tupitem = NULL;		/* this row is not visible */
		else if (kcxt.e.errcode != StromError_Success)
			tupitem = NULL;		/* chunk will raise an error */
	}
	else
	{
		tupitem = NULL;			/* kds_index - out of range */
	}


	/*
	 * Projection , thread for invisible rows must be called to 
	 *
	 *
	 *
	 *
	 */
	assert(kds_dst->format == KDS_FORMAT_ROW ||
		   kds_dst->format == KDS_FORMAT_SLOT);
	if (kds_dst->format == KDS_FORMAT_ROW)
		gpuscan_projection_row(kgpuscan, &kcxt, kds_dst, kds_src, kds_index);
	else
		gpuscan_projection_slot(kgpuscan, &kcxt, kds_dst, kds_src, kds_index);

	/*
	 * write back error status, if any
	 */
	kern_writeback_error_status(&kgpuscan->errcode, kcxt.e);
}

#endif	/* __CUDACC__ */
#endif	/* CUDA_GPUSCAN_H */
