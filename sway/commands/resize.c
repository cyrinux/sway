#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wlc/wlc.h>
#include "sway/commands.h"
#include "sway/layout.h"
#include "sway/focus.h"
#include "sway/input_state.h"
#include "sway/handlers.h"
#include "log.h"

enum resize_dim_types {
	RESIZE_DIM_PX,
	RESIZE_DIM_PPT,
	RESIZE_DIM_DEFAULT,
};

static bool set_size_floating(int new_dimension, bool use_width) {
	swayc_t *view = get_focused_float(swayc_active_workspace());
	if (view) {
		if (use_width) {
			int current_width = view->width;
			view->desired_width = new_dimension;
			floating_view_sane_size(view);

			int new_x = view->x + (int)(((view->desired_width - current_width) / 2) * -1);
			view->width = view->desired_width;
			view->x = new_x;

			update_geometry(view);
		} else {
			int current_height = view->height;
			view->desired_height = new_dimension;
			floating_view_sane_size(view);

			int new_y = view->y + (int)(((view->desired_height - current_height) / 2) * -1);
			view->height = view->desired_height;
			view->y = new_y;

			update_geometry(view);
		}

		return true;
	}

	return false;
}

static bool resize_floating(int amount, bool use_width) {
	swayc_t *view = get_focused_float(swayc_active_workspace());

	if (view) {
		if (use_width) {
			return set_size_floating(view->width + amount, true);
		} else {
			return set_size_floating(view->height + amount, false);
		}
	}

	return false;
}

/**
 * returns the index of the container's child that is first in a group.
 * This index is > to the <after> argument.
 * This makes the function usable to walk through the groups in a container.
 */
static int next_group_index(swayc_t *container, int after) {
	if (after < 0) {
		return 0;
	} else if (is_auto_layout(container->layout)) {
		if ((uint_fast32_t) after < container->nb_master) {
			return container->nb_master;
		} else {
			uint_fast32_t grp_idx = 0;
			for (int i = container->nb_master; i < container->children->length; ) {
				uint_fast32_t grp_sz = (container->children->length - i) /
					(container->nb_slave_groups - grp_idx);
				if (after - i < (int) grp_sz) {
					return i + grp_sz;
				}
				i += grp_sz;
			}
			return container->children->length;
		}
	} else {
		//		return after + 1;
		return container->children->length;
	}
}

/**
 * Return the number of children in the slave groups. This corresponds to the children
 * that are not members of the master group.
 */
static inline uint_fast32_t slave_count(swayc_t *container) {
	return container->children->length - container->nb_master;

}

/**
 * given the index of a container's child, return the index of the first child of the group
 * which index is a member of.
 */
static int group_start_index(swayc_t *container, int index) {
	if (index < 0 || ! is_auto_layout(container->layout) || (uint_fast32_t) index < container->nb_master) {
		return 0;
	} else {
		uint_fast32_t grp_sz = slave_count(container) / container->nb_slave_groups;
		uint_fast32_t remainder = slave_count(container) % container->nb_slave_groups;
		if ((index - container->nb_master) / grp_sz < container->nb_slave_groups - remainder) {
			return ((index - container->nb_master) / grp_sz) * grp_sz + container->nb_master;
		} else {
			int idx2 = (container->nb_slave_groups - remainder) * grp_sz + container->nb_master;
			return idx2 + ((idx2 - index) / (grp_sz + 1)) * (grp_sz + 1);
		}
	}
}

/**
 * given the index of a container's child, return the index of the first child of the group
 * that follows the one which index is a member of.
 */
static int group_end_index(swayc_t *container, int index) {
	if (index < 0 || ! is_auto_layout(container->layout)) {
		return container->children->length;
	} else {
		uint_fast32_t grp_sz = slave_count(container) / container->nb_slave_groups;
		uint_fast32_t remainder = slave_count(container) % container->nb_slave_groups;
		if ((index - container->nb_master) / grp_sz < container->nb_slave_groups - remainder) {
			return ((index - container->nb_master) / grp_sz + 1) * grp_sz + container->nb_master;
		} else {
			int idx2 = (container->nb_slave_groups - remainder) * grp_sz + container->nb_master;
			return idx2 + ((idx2 - index) / (grp_sz + 1) + 1) * (grp_sz + 1);
		}
	}
}

/**
 * Return the combined number of master and slave groups in the container.
 */
static inline uint_fast32_t group_count(swayc_t *container) {
	return MIN(container->nb_slave_groups, slave_count(container)) + (container->nb_master ? 1 : 0);
}

/**
 * return the index of the Group containing <index>th child of <container>.
 * The index is the order of the group along the container's major axis (starting at 0).
 */
static uint_fast32_t group_index(swayc_t *container, int index) {
	bool master_first = (container->layout == L_AUTO_LEFT || container->layout == L_AUTO_TOP);
	int nb_slaves = slave_count(container);
	if (index < (int) container->nb_master) {
		if (master_first || nb_slaves <= 0) {
			return 0;
		} else {
			return MIN(container->nb_slave_groups, nb_slaves);
		}
	} else {
		uint_fast32_t grp_idx = 0;
		for (int i = container->nb_master; i < container->children->length; ) {
			uint_fast32_t grp_sz = (container->children->length - i) /
				(container->nb_slave_groups - grp_idx);
			if (index - i < (int) grp_sz) {
				break;
			}
		}
		return grp_idx + (master_first ? 1 : 0);
	}
}

static bool resize_tiled(int amount, bool use_width) {
	swayc_t *container = get_focused_view(swayc_active_workspace());
	swayc_t *parent = container->parent;
	int idx_focused = 0;
	bool use_major = false;
	uint_fast32_t nb_before = 0;
	uint_fast32_t nb_after = 0;

	// 1. Identify a container ancestor that will allow the focused child to grow in the requested
	//    direction.
	while (container->parent) {
		parent = container->parent;
		if ((parent->children && parent->children->length > 1) &&
		    (is_auto_layout(parent->layout) || (use_width ? parent->layout == L_HORIZ :
							parent->layout == L_VERT))) {
			// check if container has siblings that can provide/absorb the space needed for
			// the resize operation.
			use_major = use_width
				? parent->layout == L_AUTO_LEFT || parent->layout == L_AUTO_RIGHT
				: parent->layout == L_AUTO_TOP || parent->layout == L_AUTO_BOTTOM;
			// Note: use_major will be false for L_HORIZ and L_VERT

			idx_focused = index_child(container);
			if (idx_focused < 0) {
				sway_log(L_ERROR, "Something weird is happening, child container not "
					 "present in its parent's children list.");
				continue;
			}
			if (use_major) {
				nb_before = group_index(parent, idx_focused);
				nb_after = group_count(parent) - nb_before - 1;
			} else {
				nb_before = idx_focused - group_start_index(parent, idx_focused);
				nb_after = next_group_index(parent, idx_focused) - idx_focused - 1;
			}
			if (nb_before || nb_after) {
				break;
			}
		}
		container = parent; /* continue up the tree to the next ancestor */
	}
	if (parent == &root_container) {
		return true;
	}
	sway_log(L_DEBUG, "Found the proper parent: %p. It has %" PRIuFAST32 " before conts, and %"
		 PRIuFAST32 " after conts", parent, nb_before, nb_after);
	// 2. Ensure that the resize operation will not make one of the resized containers drop
	//    below the "sane" size threshold.
	bool valid = true;
	swayc_t *focused = parent->children->items[idx_focused];
	int start = use_major ? 0 : group_start_index(parent, idx_focused);
	int end = use_major ? parent->children->length : group_end_index(parent, idx_focused);
	for (int i = start; i < end; ) {
		swayc_t *sibling = parent->children->items[i];
		double pixels = amount;
		bool is_before = use_width ? sibling->x < focused->x : sibling->y < focused->y;
		bool is_after  = use_width ? sibling->x > focused->x : sibling->y > focused->y;
		if (is_before || is_after) {
			pixels = -pixels;
			pixels /= is_before ? nb_before : nb_after;
			if (nb_after != 0 && nb_before != 0) {
				pixels /= 2;
			}
		}
		if (use_width ?
		    sibling->width + pixels < min_sane_w :
		    sibling->height + pixels < min_sane_h) {
			valid = false;
			break;
		}
		i = use_major ? next_group_index(parent, i) : (i + 1);
	}
	// 3. Apply the size change
	if (valid) {
		for (int i = 0; i < parent->children->length; ++i) {
			swayc_t *sibling = parent->children->items[i];
			double pixels = amount;
			bool is_before = use_width ? sibling->x < focused->x : sibling->y < focused->y;
			bool is_after  = use_width ? sibling->x > focused->x : sibling->y > focused->y;
			if (is_before || is_after) {
				pixels = -pixels;
				pixels /= is_before ? nb_before : nb_after;
				if (nb_after != 0 && nb_before != 0) {
					pixels /= 2;
				}
				sway_log(L_DEBUG, "%p: %s", sibling, is_before ? "before" : "after");
				recursive_resize(sibling, pixels,
						 use_width ?
						 (is_before ? WLC_RESIZE_EDGE_RIGHT : WLC_RESIZE_EDGE_LEFT) :
						 (is_before ? WLC_RESIZE_EDGE_BOTTOM : WLC_RESIZE_EDGE_TOP));
			} else {
				sway_log(L_DEBUG, "%p: same pos", sibling);
				recursive_resize(sibling, pixels,
						 use_width ? WLC_RESIZE_EDGE_LEFT : WLC_RESIZE_EDGE_TOP);
				recursive_resize(sibling, pixels,
						 use_width ? WLC_RESIZE_EDGE_RIGHT : WLC_RESIZE_EDGE_BOTTOM);
			}
		}
		// Recursive resize does not handle positions, let arrange_windows
		// take care of that.
		arrange_windows(swayc_active_workspace(), -1, -1);
	}
	return true;
}

static bool set_size_tiled(int amount, bool use_width) {
	int desired;
	swayc_t *focused = get_focused_view(swayc_active_workspace());

	if (use_width) {
		desired = amount - focused->width;
	} else {
		desired = amount - focused->height;
	}

	return resize_tiled(desired, use_width);
}

static bool set_size(int dimension, bool use_width) {
	swayc_t *focused = get_focused_view_include_floating(swayc_active_workspace());

	if (focused) {
		if (focused->is_floating) {
			return set_size_floating(dimension, use_width);
		} else {
			return set_size_tiled(dimension, use_width);
		}
	}

	return false;
}

static bool resize(int dimension, bool use_width, enum resize_dim_types dim_type) {
	swayc_t *focused = get_focused_view_include_floating(swayc_active_workspace());

	// translate "10 ppt" (10%) to appropriate # of pixels in case we need it
	float ppt_dim = (float)dimension / 100;

	if (use_width) {
		ppt_dim = focused->width * ppt_dim;
	} else {
		ppt_dim = focused->height * ppt_dim;
	}

	if (focused) {
		if (focused->is_floating) {
			// floating view resize dimensions should default to px, so only
			// use ppt if specified
			if (dim_type == RESIZE_DIM_PPT) {
				dimension = (int)ppt_dim;
			}

			return resize_floating(dimension, use_width);
		} else {
			// tiled view resize dimensions should default to ppt, so only use
			// px if specified
			if (dim_type != RESIZE_DIM_PX) {
				dimension = (int)ppt_dim;
			}

			return resize_tiled(dimension, use_width);
		}
	}

	return false;
}

static struct cmd_results *cmd_resize_set(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "resize set", EXPECTED_AT_LEAST, 2))) {
		return error;
	}

	if (strcasecmp(argv[0], "width") == 0 || strcasecmp(argv[0], "height") == 0) {
		// handle `reset set width 100 px height 100 px` syntax, also allows
		// specifying only one dimension for a `resize set`
		int cmd_num = 0;
		int dim;

		while ((cmd_num + 1) < argc) {
			dim = (int)strtol(argv[cmd_num + 1], NULL, 10);
			if (errno == ERANGE || dim == 0) {
				errno = 0;
				return cmd_results_new(CMD_INVALID, "resize set",
					"Expected 'resize set <width|height> <amount> [px] [<width|height> <amount> [px]]'");
			}

			if (strcasecmp(argv[cmd_num], "width") == 0) {
				set_size(dim, true);
			} else if (strcasecmp(argv[cmd_num], "height") == 0) {
				set_size(dim, false);
			} else {
				return cmd_results_new(CMD_INVALID, "resize set",
					"Expected 'resize set <width|height> <amount> [px] [<width|height> <amount> [px]]'");
			}

			cmd_num += 2;

			if (cmd_num < argc && strcasecmp(argv[cmd_num], "px") == 0) {
				// if this was `resize set width 400 px height 300 px`, disregard the `px` arg
				cmd_num++;
			}
		}
	} else {
		// handle `reset set 100 px 100 px` syntax
		int width = (int)strtol(argv[0], NULL, 10);
		if (errno == ERANGE || width == 0) {
			errno = 0;
			return cmd_results_new(CMD_INVALID, "resize set",
				"Expected 'resize set <width> [px] <height> [px]'");
		}

		int height_arg = 1;
		if (strcasecmp(argv[1], "px") == 0) {
			height_arg = 2;
		}

		int height = (int)strtol(argv[height_arg], NULL, 10);
		if (errno == ERANGE || height == 0) {
			errno = 0;
			return cmd_results_new(CMD_INVALID, "resize set",
				"Expected 'resize set <width> [px] <height> [px]'");
		}

		set_size(width, true);
		set_size(height, false);
	}

	return cmd_results_new(CMD_SUCCESS, NULL, NULL);
}

struct cmd_results *cmd_resize(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if (config->reading) return cmd_results_new(CMD_FAILURE, "resize", "Can't be used in config file.");
	if (!config->active) return cmd_results_new(CMD_FAILURE, "resize", "Can only be used when sway is running.");

	if (strcasecmp(argv[0], "set") == 0) {
		return cmd_resize_set(argc - 1, &argv[1]);
	}

	if ((error = checkarg(argc, "resize", EXPECTED_AT_LEAST, 2))) {
		return error;
	}

	int dim_arg = argc - 1;

	enum resize_dim_types dim_type = RESIZE_DIM_DEFAULT;
	if (strcasecmp(argv[dim_arg], "ppt") == 0) {
		dim_type = RESIZE_DIM_PPT;
		dim_arg--;
	} else if (strcasecmp(argv[dim_arg], "px") == 0) {
		dim_type = RESIZE_DIM_PX;
		dim_arg--;
	}

	int amount = (int)strtol(argv[dim_arg], NULL, 10);
	if (errno == ERANGE || amount == 0) {
		errno = 0;
		amount = 10; // this is the default resize dimension used by i3 for both px and ppt
		sway_log(L_DEBUG, "Tried to get resize dimension out of '%s' but failed; setting dimension to default %d",
			argv[dim_arg], amount);
	}

	bool use_width = false;
	if (strcasecmp(argv[1], "width") == 0) {
		use_width = true;
	} else if (strcasecmp(argv[1], "height") != 0) {
		return cmd_results_new(CMD_INVALID, "resize",
			"Expected 'resize <shrink|grow> <width|height> [<amount>] [px|ppt]'");
	}

	if (strcasecmp(argv[0], "shrink") == 0) {
		amount *= -1;
	} else if (strcasecmp(argv[0], "grow") != 0) {
		return cmd_results_new(CMD_INVALID, "resize",
			"Expected 'resize <shrink|grow> <width|height> [<amount>] [px|ppt]'");
	}

	resize(amount, use_width, dim_type);
	return cmd_results_new(CMD_SUCCESS, NULL, NULL);
}
