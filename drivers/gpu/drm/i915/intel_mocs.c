/*
 * Copyright (c) 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions: *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "intel_mocs.h"
#include "intel_lrc.h"
#include "intel_ringbuffer.h"

/* structures required */
struct drm_i915_mocs_entry {
	u32 control_value;
	u16 l3cc_value;
	u16 used;
};

struct drm_i915_mocs_table {
	unsigned int size;
	unsigned int n_entries;
	const struct drm_i915_mocs_entry *table;
};

/* Defines for the tables (XXX_MOCS_0 - XXX_MOCS_63) */
#define _LE_CACHEABILITY(value)	((value) << 0)
#define _LE_TGT_CACHE(value)	((value) << 2)
#define LE_LRUM(value)		((value) << 4)
#define LE_AOM(value)		((value) << 6)
#define LE_RSC(value)		((value) << 7)
#define LE_SCC(value)		((value) << 8)
#define LE_PFM(value)		((value) << 11)
#define LE_SCF(value)		((value) << 14)
#define LE_COS(value)		((value) << 15)
#define LE_SSE(value)		((value) << 17)

/* Defines for the tables (LNCFMOCS0 - LNCFMOCS31) - two entries per word */
#define L3_ESC(value)		((value) << 0)
#define L3_SCC(value)		((value) << 1)
#define _L3_CACHEABILITY(value)	((value) << 4)

/* Helper defines */
#define GEN9_NUM_MOCS_ENTRIES	62  /* 62 out of 64 - 63 & 64 are reserved. */
#define GEN11_NUM_MOCS_ENTRIES	64  /* 63-64 are reserved, but configured. */

/* (e)LLC caching options */
#define LE_0_PAGETABLE		_LE_CACHEABILITY(0)
#define LE_1_UC			_LE_CACHEABILITY(1)
#define LE_2_WT			_LE_CACHEABILITY(2)
#define LE_3_WB			_LE_CACHEABILITY(3)

/* Target cache */
#define LE_TC_0_PAGETABLE	_LE_TGT_CACHE(0)
#define LE_TC_1_LLC		_LE_TGT_CACHE(1)
#define LE_TC_2_LLC_ELLC	_LE_TGT_CACHE(2)
#define LE_TC_3_LLC_ELLC_ALT	_LE_TGT_CACHE(3)

/* L3 caching options */
#define L3_0_DIRECT		_L3_CACHEABILITY(0)
#define L3_1_UC			_L3_CACHEABILITY(1)
#define L3_2_RESERVED		_L3_CACHEABILITY(2)
#define L3_3_WB			_L3_CACHEABILITY(3)

#define MOCS_ENTRY(__idx, __control_value, __l3cc_value) \
	[__idx] = { \
		.control_value = __control_value, \
		.l3cc_value = __l3cc_value, \
		.used = 1, \
	}

/*
 * MOCS tables
 *
 * These are the MOCS tables that are programmed across all the rings.
 * The control value is programmed to all the rings that support the
 * MOCS registers. While the l3cc_values are only programmed to the
 * LNCFCMOCS0 - LNCFCMOCS32 registers.
 *
 * These tables are intended to be kept reasonably consistent across
 * HW platforms, and for ICL+, be identical across OSes. To achieve
 * that, for Icelake and above, list of entries is published as part
 * of bspec.
 *
 * Entries not part of the following tables are undefined as far as
 * userspace is concerned and shouldn't be relied upon.  For the time
 * being they will be initialized to PTE.
 *
 * The last two entries are reserved by the hardware. For ICL+ they
 * should be initialized according to bspec and never used, for older
 * platforms they should never be written to.
 *
 * NOTE: These tables are part of bspec and defined as part of hardware
 *       interface for ICL+. For older platforms, they are part of kernel
 *       ABI. It is expected that, for specific hardware platform, existing
 *       entries will remain constant and the table will only be updated by
 *       adding new entries, filling unused positions.
 */
#define GEN9_MOCS_ENTRIES \
	MOCS_ENTRY(I915_MOCS_UNCACHED, \
		   LE_1_UC | LE_TC_2_LLC_ELLC, \
		   L3_1_UC), \
	MOCS_ENTRY(I915_MOCS_PTE, \
		   LE_0_PAGETABLE | LE_TC_2_LLC_ELLC | LE_LRUM(3), \
		   L3_3_WB)

static const struct drm_i915_mocs_entry skylake_mocs_table[] = {
	GEN9_MOCS_ENTRIES,
	MOCS_ENTRY(I915_MOCS_CACHED,
		   LE_3_WB | LE_TC_2_LLC_ELLC | LE_LRUM(3),
		   L3_3_WB)
};

/* NOTE: the LE_TGT_CACHE is not used on Broxton */
static const struct drm_i915_mocs_entry broxton_mocs_table[] = {
	GEN9_MOCS_ENTRIES,
	MOCS_ENTRY(I915_MOCS_CACHED,
		   LE_1_UC | LE_TC_2_LLC_ELLC | LE_LRUM(3),
		   L3_3_WB)
};

#define GEN11_MOCS_ENTRIES \
	/* Base - Uncached (Deprecated) */ \
	MOCS_ENTRY(I915_MOCS_UNCACHED, \
		   LE_1_UC | LE_TC_1_LLC, \
		   L3_1_UC), \
	/* Base - L3 + LeCC:PAT (Deprecated) */ \
	MOCS_ENTRY(I915_MOCS_PTE, \
		   LE_0_PAGETABLE | LE_TC_1_LLC, \
		   L3_3_WB), \
	/* Base - L3 + LLC */ \
	MOCS_ENTRY(2, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), \
		   L3_3_WB), \
	/* Base - Uncached */ \
	MOCS_ENTRY(3, \
		   LE_1_UC | LE_TC_1_LLC, \
		   L3_1_UC), \
	/* Base - L3 */ \
	MOCS_ENTRY(4, \
		   LE_1_UC | LE_TC_1_LLC, \
		   L3_3_WB), \
	/* Base - LLC */ \
	MOCS_ENTRY(5, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), \
		   L3_1_UC), \
	/* Age 0 - LLC */ \
	MOCS_ENTRY(6, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(1), \
		   L3_1_UC), \
	/* Age 0 - L3 + LLC */ \
	MOCS_ENTRY(7, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(1), \
		   L3_3_WB), \
	/* Age: Don't Chg. - LLC */ \
	MOCS_ENTRY(8, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(2), \
		   L3_1_UC), \
	/* Age: Don't Chg. - L3 + LLC */ \
	MOCS_ENTRY(9, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(2), \
		   L3_3_WB), \
	/* No AOM - LLC */ \
	MOCS_ENTRY(10, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_AOM(1), \
		   L3_1_UC), \
	/* No AOM - L3 + LLC */ \
	MOCS_ENTRY(11, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_AOM(1), \
		   L3_3_WB), \
	/* No AOM; Age 0 - LLC */ \
	MOCS_ENTRY(12, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(1) | LE_AOM(1), \
		   L3_1_UC), \
	/* No AOM; Age 0 - L3 + LLC */ \
	MOCS_ENTRY(13, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(1) | LE_AOM(1), \
		   L3_3_WB), \
	/* No AOM; Age:DC - LLC */ \
	MOCS_ENTRY(14, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(2) | LE_AOM(1), \
		   L3_1_UC), \
	/* No AOM; Age:DC - L3 + LLC */ \
	MOCS_ENTRY(15, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(2) | LE_AOM(1), \
		   L3_3_WB), \
	/* Self-Snoop - L3 + LLC */ \
	MOCS_ENTRY(18, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_SSE(3), \
		   L3_3_WB), \
	/* Skip Caching - L3 + LLC(12.5%) */ \
	MOCS_ENTRY(19, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_SCC(7), \
		   L3_3_WB), \
	/* Skip Caching - L3 + LLC(25%) */ \
	MOCS_ENTRY(20, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_SCC(3), \
		   L3_3_WB), \
	/* Skip Caching - L3 + LLC(50%) */ \
	MOCS_ENTRY(21, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_SCC(1), \
		   L3_3_WB), \
	/* Skip Caching - L3 + LLC(75%) */ \
	MOCS_ENTRY(22, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_RSC(1) | LE_SCC(3), \
		   L3_3_WB), \
	/* Skip Caching - L3 + LLC(87.5%) */ \
	MOCS_ENTRY(23, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_RSC(1) | LE_SCC(7), \
		   L3_3_WB), \
	/* HW Reserved - SW program but never use */ \
	MOCS_ENTRY(62, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), \
		   L3_1_UC), \
	/* HW Reserved - SW program but never use */ \
	MOCS_ENTRY(63, \
		   LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), \
		   L3_1_UC)

static const struct drm_i915_mocs_entry icelake_mocs_table[] = {
	GEN11_MOCS_ENTRIES
};

/**
 * get_mocs_settings()
 * @dev_priv:	i915 device.
 * @table:      Output table that will be made to point at appropriate
 *	      MOCS values for the device.
 *
 * This function will return the values of the MOCS table that needs to
 * be programmed for the platform. It will return the values that need
 * to be programmed and if they need to be programmed.
 *
 * Return: true if there are applicable MOCS settings for the device.
 */
static bool get_mocs_settings(struct drm_i915_private *dev_priv,
			      struct drm_i915_mocs_table *table)
{
	bool result = false;

	if (INTEL_GEN(dev_priv) >= 11) {
		table->size  = ARRAY_SIZE(icelake_mocs_table);
		table->table = icelake_mocs_table;
		table->n_entries = GEN11_NUM_MOCS_ENTRIES;
		result = true;
	} else if (IS_GEN9_BC(dev_priv) || IS_CANNONLAKE(dev_priv)) {
		table->size  = ARRAY_SIZE(skylake_mocs_table);
		table->n_entries = GEN9_NUM_MOCS_ENTRIES;
		table->table = skylake_mocs_table;
		result = true;
	} else if (IS_GEN9_LP(dev_priv)) {
		table->size  = ARRAY_SIZE(broxton_mocs_table);
		table->n_entries = GEN9_NUM_MOCS_ENTRIES;
		table->table = broxton_mocs_table;
		result = true;
	} else {
		WARN_ONCE(INTEL_GEN(dev_priv) >= 9,
			  "Platform that should have a MOCS table does not.\n");
	}

	/* WaDisableSkipCaching:skl,bxt,kbl,glk */
	if (IS_GEN(dev_priv, 9)) {
		int i;

		for (i = 0; i < table->size; i++)
			if (WARN_ON(table->table[i].l3cc_value &
				    (L3_ESC(1) | L3_SCC(0x7))))
				return false;
	}

	return result;
}

static i915_reg_t mocs_register(enum intel_engine_id engine_id, int index)
{
	switch (engine_id) {
	case RCS0:
		return GEN9_GFX_MOCS(index);
	case VCS0:
		return GEN9_MFX0_MOCS(index);
	case BCS0:
		return GEN9_BLT_MOCS(index);
	case VECS0:
		return GEN9_VEBOX_MOCS(index);
	case VCS1:
		return GEN9_MFX1_MOCS(index);
	case VCS2:
		return GEN11_MFX2_MOCS(index);
	default:
		MISSING_CASE(engine_id);
		return INVALID_MMIO_REG;
	}
}

/*
 * Get control_value from MOCS entry taking into account when it's not used:
 * I915_MOCS_PTE's value is returned in this case.
 */
static u32 get_entry_control(const struct drm_i915_mocs_table *table,
			     unsigned int index)
{
	if (table->table[index].used)
		return table->table[index].control_value;

	return table->table[I915_MOCS_PTE].control_value;
}

/**
 * intel_mocs_init_engine() - emit the mocs control table
 * @engine:	The engine for whom to emit the registers.
 *
 * This function simply emits a MI_LOAD_REGISTER_IMM command for the
 * given table starting at the given address.
 */
void intel_mocs_init_engine(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	struct drm_i915_mocs_table table;
	unsigned int index;
	u32 unused_value;

	if (!get_mocs_settings(dev_priv, &table))
		return;

	/* Set unused values to PTE */
	unused_value = table.table[I915_MOCS_PTE].control_value;

	for (index = 0; index < table.size; index++) {
		u32 value = get_entry_control(&table, index);

		I915_WRITE(mocs_register(engine->id, index), value);
	}

	/* All remaining entries are also unused */
	for (; index < table.n_entries; index++)
		I915_WRITE(mocs_register(engine->id, index), unused_value);
}

/**
 * emit_mocs_control_table() - emit the mocs control table
 * @rq:	Request to set up the MOCS table for.
 * @table:	The values to program into the control regs.
 *
 * This function simply emits a MI_LOAD_REGISTER_IMM command for the
 * given table starting at the given address.
 *
 * Return: 0 on success, otherwise the error status.
 */
static int emit_mocs_control_table(struct i915_request *rq,
				   const struct drm_i915_mocs_table *table)
{
	enum intel_engine_id engine = rq->engine->id;
	unsigned int index;
	u32 unused_value;
	u32 *cs;

	if (GEM_WARN_ON(table->size > table->n_entries))
		return -ENODEV;

	/* Set unused values to PTE */
	unused_value = table->table[I915_MOCS_PTE].control_value;

	cs = intel_ring_begin(rq, 2 + 2 * table->n_entries);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = MI_LOAD_REGISTER_IMM(table->n_entries);

	for (index = 0; index < table->size; index++) {
		u32 value = get_entry_control(table, index);

		*cs++ = i915_mmio_reg_offset(mocs_register(engine, index));
		*cs++ = value;
	}

	/* All remaining entries are also unused */
	for (; index < table->n_entries; index++) {
		*cs++ = i915_mmio_reg_offset(mocs_register(engine, index));
		*cs++ = unused_value;
	}

	*cs++ = MI_NOOP;
	intel_ring_advance(rq, cs);

	return 0;
}

/*
 * Get l3cc_value from MOCS entry taking into account when it's not used:
 * I915_MOCS_PTE's value is returned in this case.
 */
static u16 get_entry_l3cc(const struct drm_i915_mocs_table *table,
			  unsigned int index)
{
	if (table->table[index].used)
		return table->table[index].l3cc_value;

	return table->table[I915_MOCS_PTE].l3cc_value;
}

static inline u32 l3cc_combine(const struct drm_i915_mocs_table *table,
			       u16 low,
			       u16 high)
{
	return low | high << 16;
}

/**
 * emit_mocs_l3cc_table() - emit the mocs control table
 * @rq:	Request to set up the MOCS table for.
 * @table:	The values to program into the control regs.
 *
 * This function simply emits a MI_LOAD_REGISTER_IMM command for the
 * given table starting at the given address. This register set is
 * programmed in pairs.
 *
 * Return: 0 on success, otherwise the error status.
 */
static int emit_mocs_l3cc_table(struct i915_request *rq,
				const struct drm_i915_mocs_table *table)
{
	u16 unused_value;
	unsigned int i;
	u32 *cs;

	if (GEM_WARN_ON(table->size > table->n_entries))
		return -ENODEV;

	/* Set unused values to PTE */
	unused_value = table->table[I915_MOCS_PTE].l3cc_value;

	cs = intel_ring_begin(rq, 2 + table->n_entries);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = MI_LOAD_REGISTER_IMM(table->n_entries / 2);

	for (i = 0; i < table->size / 2; i++) {
		u16 low = get_entry_l3cc(table, 2 * i);
		u16 high = get_entry_l3cc(table, 2 * i + 1);

		*cs++ = i915_mmio_reg_offset(GEN9_LNCFCMOCS(i));
		*cs++ = l3cc_combine(table, low, high);
	}

	/* Odd table size - 1 left over */
	if (table->size & 0x01) {
		u16 low = get_entry_l3cc(table, 2 * i);

		*cs++ = i915_mmio_reg_offset(GEN9_LNCFCMOCS(i));
		*cs++ = l3cc_combine(table, low, unused_value);
		i++;
	}

	/* All remaining entries are also unused */
	for (; i < table->n_entries / 2; i++) {
		*cs++ = i915_mmio_reg_offset(GEN9_LNCFCMOCS(i));
		*cs++ = l3cc_combine(table, unused_value, unused_value);
	}

	*cs++ = MI_NOOP;
	intel_ring_advance(rq, cs);

	return 0;
}

/**
 * intel_mocs_init_l3cc_table() - program the mocs control table
 * @dev_priv:      i915 device private
 *
 * This function simply programs the mocs registers for the given table
 * starting at the given address. This register set is  programmed in pairs.
 *
 * These registers may get programmed more than once, it is simpler to
 * re-program 32 registers than maintain the state of when they were programmed.
 * We are always reprogramming with the same values and this only on context
 * start.
 *
 * Return: Nothing.
 */
void intel_mocs_init_l3cc_table(struct drm_i915_private *dev_priv)
{
	struct drm_i915_mocs_table table;
	unsigned int i;
	u16 unused_value;

	if (!get_mocs_settings(dev_priv, &table))
		return;

	/* Set unused values to PTE */
	unused_value = table.table[I915_MOCS_PTE].l3cc_value;

	for (i = 0; i < table.size / 2; i++) {
		u16 low = get_entry_l3cc(&table, 2 * i);
		u16 high = get_entry_l3cc(&table, 2 * i + 1);

		I915_WRITE(GEN9_LNCFCMOCS(i),
			   l3cc_combine(&table, low, high));
	}

	/* Odd table size - 1 left over */
	if (table.size & 0x01) {
		u16 low = get_entry_l3cc(&table, 2 * i);

		I915_WRITE(GEN9_LNCFCMOCS(i),
			   l3cc_combine(&table, low, unused_value));
		i++;
	}

	/* All remaining entries are also unused */
	for (; i < table.n_entries / 2; i++)
		I915_WRITE(GEN9_LNCFCMOCS(i),
			   l3cc_combine(&table, unused_value, unused_value));
}

/**
 * intel_rcs_context_init_mocs() - program the MOCS register.
 * @rq:	Request to set up the MOCS tables for.
 *
 * This function will emit a batch buffer with the values required for
 * programming the MOCS register values for all the currently supported
 * rings.
 *
 * These registers are partially stored in the RCS context, so they are
 * emitted at the same time so that when a context is created these registers
 * are set up. These registers have to be emitted into the start of the
 * context as setting the ELSP will re-init some of these registers back
 * to the hw values.
 *
 * Return: 0 on success, otherwise the error status.
 */
int intel_rcs_context_init_mocs(struct i915_request *rq)
{
	struct drm_i915_mocs_table t;
	int ret;

	if (get_mocs_settings(rq->i915, &t)) {
		/* Program the RCS control registers */
		ret = emit_mocs_control_table(rq, &t);
		if (ret)
			return ret;

		/* Now program the l3cc registers */
		ret = emit_mocs_l3cc_table(rq, &t);
		if (ret)
			return ret;
	}

	return 0;
}
