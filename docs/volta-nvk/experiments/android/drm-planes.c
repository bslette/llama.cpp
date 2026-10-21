// List DRM planes: id, type, possible_crtcs, crtc, formats. Raw ioctls, no libdrm.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static int fd;
static void prop_type(uint32_t plane, char *out) {
	struct drm_mode_obj_get_properties gp = {.obj_id = plane, .obj_type = DRM_MODE_OBJECT_PLANE};
	ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp);
	uint32_t *ids = calloc(gp.count_props, 4); uint64_t *vals = calloc(gp.count_props, 8);
	gp.props_ptr = (uintptr_t)ids; gp.prop_values_ptr = (uintptr_t)vals;
	ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp);
	strcpy(out, "?");
	for (uint32_t i = 0; i < gp.count_props; i++) {
		struct drm_mode_get_property p = {.prop_id = ids[i]};
		ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p);
		if (!strcmp(p.name, "type")) sprintf(out, "%s", vals[i] == 0 ? "overlay" : vals[i] == 1 ? "primary" : "cursor");
	}
}
int main(int argc, char **argv) {
	fd = open(argc > 1 ? argv[1] : "/dev/dri/card0", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	struct drm_set_client_cap cap = {DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1};
	ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
	struct drm_mode_card_res res = {0};
	ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
	uint32_t *crtcs = calloc(res.count_crtcs, 4);
	struct drm_mode_card_res r2 = {.crtc_id_ptr = (uintptr_t)crtcs, .count_crtcs = res.count_crtcs};
	ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2);
	printf("crtcs:"); for (uint32_t i = 0; i < res.count_crtcs; i++) printf(" %u", crtcs[i]); printf("\n");
	struct drm_mode_get_plane_res pr = {0};
	ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
	uint32_t *planes = calloc(pr.count_planes, 4);
	pr.plane_id_ptr = (uintptr_t)planes;
	ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
	for (uint32_t i = 0; i < pr.count_planes; i++) {
		struct drm_mode_get_plane p = {.plane_id = planes[i]};
		ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p);
		uint32_t *fmts = calloc(p.count_format_types, 4);
		p.format_type_ptr = (uintptr_t)fmts;
		ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p);
		char t[16]; prop_type(planes[i], t);
		printf("plane %u %-7s possible_crtcs=0x%x crtc=%u fmts=%u:", planes[i], t, p.possible_crtcs, p.crtc_id, p.count_format_types);
		for (uint32_t k = 0; k < p.count_format_types && k < 12; k++) printf(" %.4s", (char *)&fmts[k]);
		printf("\n");
	}
	return 0;
}
