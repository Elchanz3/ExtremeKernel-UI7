/* SPDX-License-Identifier: GPL-2.0 */

/*
 * (C) COPYRIGHT 2021 Samsung Electronics Inc. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 */

#include <linux/device.h>
#include <linux/of.h>
#include <linux/sysfs.h>
#include <linux/slab.h>

#include <gpex_utils.h>
#include <gpexbe_devicetree.h>

/* The bootloader ECT is the sole source of the G3D DVFS table (see
 * build_clk_table). cal-if exposes the ECT's dvfs_g3d levels and voltages. */
#include <soc/samsung/cal-if.h>

#define CPU_MAX INT_MAX
#define ECT_G3D_MAX_LEVELS 48

/* Governor defaults applied to the ECT-built levels. The device tree's
 * per-level thresholds/QoS are intentionally not read. */
#define GPU_DEF_MIN_THRESHOLD 30
#define GPU_DEF_MAX_THRESHOLD 85
#define GPU_DEF_DOWN_STAYCOUNT 2
static gpu_dt dt_info;
static struct _dt_clock_item *clock_table;
static struct _dt_clqos_item *clqos_table;

static int gpexbe_devicetree_read_u32(const char *of_string, u32 *of_data)
{
	int ret = 0;

	if (!of_string || !of_data) {
		GPU_LOG(MALI_EXYNOS_ERROR, "NULL: failed to get item from dt\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(dt_info.dev->of_node, of_string, of_data);

	if (ret) {
		GPU_LOG(MALI_EXYNOS_ERROR,
			"%s: failed to get item from dt. Data will be set to 0.\n", of_string);
		*of_data = 0;
	}

	return ret;
}

/* TODO: document in the interface header. Return value same as of_property_read_string */
static int gpexbe_devicetree_read_string(const char *of_string, const char **of_data)
{
	int ret = 0;

	if (!of_string || !of_data) {
		GPU_LOG(MALI_EXYNOS_ERROR, "NULL: failed to get item from dt\n");
		return -EINVAL;
	}

	ret = of_property_read_string(dt_info.dev->of_node, of_string, of_data);
	if (ret) {
		GPU_LOG(MALI_EXYNOS_ERROR,
			"%s: failed to get item from dt. Data will be set to NULL.\n", of_string);
		*of_data = NULL;
	}

	return ret;
}

static int gpexbe_devicetree_read_u32_array(const char *of_string, int *of_data, int sz)
{
	int ret = 0;

	if (!of_string || !of_data || sz < 0) {
		GPU_LOG(MALI_EXYNOS_ERROR, "NULL: failed to get item from dt. Invalid params\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(dt_info.dev->of_node, of_string, of_data, sz);

	if (ret)
		GPU_LOG(MALI_EXYNOS_ERROR, "%s: failed to get item from dt (u32 array)\n",
			of_string);

	return ret;
}

static int read_interactive_info_array(void)
{
	int interactive_info[3] = { 0, 0, 0 };

	gpexbe_devicetree_read_u32_array("interactive_info", interactive_info, 3);

	/* TODO: change default value to something more sensible */
	dt_info.interactive_info.highspeed_clock =
		interactive_info[0] == 0 ? 500 : (u32)interactive_info[0];
	dt_info.interactive_info.highspeed_load =
		interactive_info[1] == 0 ? 100 : (u32)interactive_info[1];
	dt_info.interactive_info.highspeed_delay =
		interactive_info[2] == 0 ? 0 : (u32)interactive_info[2];

	return 0;
}

static int build_clk_table(void)
{
	unsigned long rates[ECT_G3D_MAX_LEVELS];
	unsigned long ect_max, ect_min;
	int levels, i;

	/* The ECT (bootloader) DVFS block is the ONLY source of the G3D levels.
	 * The device-tree gpu_dvfs_table is deliberately never read. If the ECT
	 * table cannot be obtained, panic rather than silently fall back to the
	 * DT: a boot loop is recoverable, a wrong silent table is not. */
	levels = cal_dfs_get_rate_table(dt_info.g3d_cmu_cal_id, rates);
	ect_max = cal_dfs_get_max_freq(dt_info.g3d_cmu_cal_id);
	ect_min = cal_dfs_get_min_freq(dt_info.g3d_cmu_cal_id);

	if (levels <= 0 || levels > ECT_G3D_MAX_LEVELS)
		panic("gpu_dvfs: no G3D DVFS table from the ECT (levels=%d); DT fallback refused\n",
		      levels);

	for (i = 1; i < levels; i++)
		if (rates[i] >= rates[i - 1])
			panic("gpu_dvfs: ECT G3D levels not strictly descending at index %d\n", i);

	clock_table = kcalloc(levels, sizeof(*clock_table), GFP_KERNEL);
	if (!clock_table)
		return -ENOMEM;

	for (i = 0; i < levels; i++) {
		clock_table[i].clock = (int)rates[i];
		clock_table[i].min_threshold = GPU_DEF_MIN_THRESHOLD;
		clock_table[i].max_threshold = GPU_DEF_MAX_THRESHOLD;
		clock_table[i].down_staycount = GPU_DEF_DOWN_STAYCOUNT;
		clock_table[i].mem_freq = 0;              /* no MIF QoS lock */
		clock_table[i].cpu_little_min_freq = 0;
		clock_table[i].cpu_middle_min_freq = 0;
		clock_table[i].cpu_big_max_freq = CPU_MAX;
		clock_table[i].llc_ways = 0;
		GPU_LOG(MALI_EXYNOS_INFO, "ECT: G3D level %d = %d kHz\n", i,
			clock_table[i].clock);
	}

	/* Expose the whole DVFS block: cap at its own top/bottom level, not the
	 * MINMAX policy row (which was capping the higher ECT levels). */
	dt_info.gpu_dvfs_table_size.row = levels;
	dt_info.gpu_dvfs_table_size.col = 9;
	dt_info.gpu_max_clock = (u32)rates[0];
	dt_info.gpu_min_clock = (u32)rates[levels - 1];

	GPU_LOG(MALI_EXYNOS_INFO,
		"ECT: G3D DVFS from bootloader: %d levels, %u..%u kHz "
		"(MINMAX max %lu min %lu ignored; DT table ignored)\n",
		levels, dt_info.gpu_min_clock, dt_info.gpu_max_clock, ect_max, ect_min);

	return 0;
}

static int build_cl_pmqos_table(void)
{
	int array_size = dt_info.gpu_cl_pmqos_table_size.row * dt_info.gpu_cl_pmqos_table_size.col;
	u32 *raw_table;
	int row = 0;

	if (array_size <= 0) {
		int num_rows = dt_info.gpu_dvfs_table_size.row;

		clqos_table = kcalloc(num_rows, sizeof(*clqos_table), GFP_KERNEL);

		for (row = 0; row < num_rows; row++)
			clqos_table[row].clock = clock_table[row].clock;

		dt_info.gpu_cl_pmqos_table_size.row = num_rows;
		dt_info.gpu_cl_pmqos_table_size.col = 1;

		return 0;
	}

	raw_table = kcalloc(array_size, sizeof(*raw_table), GFP_KERNEL);

	if (!raw_table)
		return -ENOMEM;

	gpexbe_devicetree_read_u32_array("gpu_cl_pmqos_table", raw_table, array_size);

	clqos_table =
		kcalloc(dt_info.gpu_cl_pmqos_table_size.row, sizeof(*clqos_table), GFP_KERNEL);

	for (row = 0; row < dt_info.gpu_cl_pmqos_table_size.row; row++) {
		int table_idx = row * dt_info.gpu_cl_pmqos_table_size.col;

		clqos_table[row].clock = raw_table[table_idx];
		clqos_table[row].mif_min = raw_table[table_idx + 1];
		clqos_table[row].little_min = raw_table[table_idx + 2];
		clqos_table[row].middle_min = raw_table[table_idx + 3];
		clqos_table[row].big_max = raw_table[table_idx + 4];
	}

	kfree(raw_table);

	return 0;
}

static void read_from_dt(void)
{
	/* clock backend */
	gpexbe_devicetree_read_u32("g3d_cmu_cal_id", &dt_info.g3d_cmu_cal_id);

	/* PM backend */
	gpexbe_devicetree_read_string("g3d_genpd_name", &dt_info.g3d_genpd_name);

	/* CLOCK */
	gpexbe_devicetree_read_u32("gpu_max_clock", &dt_info.gpu_max_clock);
	gpexbe_devicetree_read_u32("gpu_min_clock", &dt_info.gpu_min_clock);
	gpexbe_devicetree_read_u32("gpu_pmqos_cpu_cluster_num", &dt_info.gpu_pmqos_cpu_cluster_num);

	gpexbe_devicetree_read_u32_array("gpu_dvfs_table_size", (int *)&dt_info.gpu_dvfs_table_size,
					 2);
	gpexbe_devicetree_read_u32_array("gpu_cl_pmqos_table_size",
					 (int *)&dt_info.gpu_cl_pmqos_table_size, 2);

	/* DEBUG_BACKEND */
	gpexbe_devicetree_read_u32("gpu_ess_id_type", &dt_info.gpu_ess_id_type);

	/* LEGACY DVFS */
	gpexbe_devicetree_read_string("governor", &dt_info.governor);
	gpexbe_devicetree_read_u32("gpu_dvfs_bl_config_clock", &dt_info.gpu_dvfs_bl_config_clock);
	gpexbe_devicetree_read_u32("gpu_dvfs_polling_time", &dt_info.gpu_dvfs_polling_time);

	/* IFPO */
	gpexbe_devicetree_read_u32("gpu_inter_frame_pm", &dt_info.gpu_inter_frame_pm);

	/* PM BACKEND */
	gpexbe_devicetree_read_u32("gpu_pmu_status_reg_offset", &dt_info.gpu_pmu_status_reg_offset);
	gpexbe_devicetree_read_u32("gpu_pmu_status_local_pwr_mask",
				   &dt_info.gpu_pmu_status_local_pwr_mask);

	/* PM */
	gpexbe_devicetree_read_u32("gpu_runtime_pm_delay_time", &dt_info.gpu_runtime_pm_delay_time);

	/* QOS */
	gpexbe_devicetree_read_u32("gpu_bts_support", &dt_info.gpu_bts_support);
	gpexbe_devicetree_read_u32("gpu_mo_min_clock", &dt_info.gpu_mo_min_clock);

	/* UTILS */
	gpexbe_devicetree_read_u32("gpu_debug_level", &dt_info.gpu_debug_level);

	/* HCM */
	gpexbe_devicetree_read_u32("gpu_heavy_compute_cpu0_min_clock;",
				   &dt_info.gpu_heavy_compute_cpu0_min_clock);
	gpexbe_devicetree_read_u32("gpu_heavy_compute_vk_cpu0_min_clock;",
				   &dt_info.gpu_heavy_compute_vk_cpu0_min_clock);

	/* TSG */
	gpexbe_devicetree_read_u32("gpu_weight_table_idx_0", &dt_info.gpu_weight_table_idx_0);
	gpexbe_devicetree_read_u32("gpu_weight_table_idx_1", &dt_info.gpu_weight_table_idx_1);
}

/******************
 * GETTERS
 * ***************/
dt_clock_item *gpexbe_devicetree_get_clock_table(void)
{
	return clock_table;
}

/********************************************
 * Helper Functions. (Must remain non static)
 * ******************************************/
int gpexbe_devicetree_get_int_internal(size_t offset, const char *str)
{
	//pr_warn("mali: %s %s %d\n", __func__, str, *(int*)((u8*)&dt_info + offset));
	//GPU_LOG(MALI_EXYNOS_INFO, "%s: %s\n", of_string, *of_data);
	return *(int *)((u8 *)&dt_info + offset);
}

char *gpexbe_devicetree_get_str_internal(size_t offset, const char *str)
{
	//pr_warn("mali: %s %s %s\n", __func__, str, *(char**)((u8*)&dt_info + offset));
	//GPU_LOG(MALI_EXYNOS_INFO, "%s: %s\n", of_string, *of_data);
	return *(char **)((u8 *)&dt_info + offset);
}

gpu_dt *gpexbe_devicetree_get_gpu_dt(void)
{
	return &dt_info;
}

/************************************************************************
 * INIT and TERM functions
 ************************************************************************/

int gpexbe_devicetree_init(struct device *dev)
{
	dt_info.dev = dev;
	read_from_dt();
	build_clk_table();
	build_cl_pmqos_table();
	read_interactive_info_array();

	dt_info.gpu_dvfs_table = clock_table;
	dt_info.gpu_cl_pmqos_table = clqos_table;

	dt_info.initialized = 1;

	return 0;
}

void gpexbe_devicetree_term(void)
{
	kfree(clock_table);
	kfree(clqos_table);

	clock_table = NULL;
	clqos_table = NULL;

	memset(&dt_info, 0, sizeof(gpu_dt));

	return;
}
