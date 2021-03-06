/*
 * drivers/video/fb_devns.c
 *
 * Derived from the mux_fb driver:
 *    Copyright (C) 2010-2011 Columbia University
 *    Author: Jeremy C. Andrus <jeremya@cs.columbia.edu>
 *
 * Copyright (c) 2011-2013 Cellrox Ltd. Certain portions are copyrighted by
 * Columbia University. This program is free software licensed under the GNU
 * General Public License Version 2 (GPL 2). You can distribute it and/or
 * modify it under the terms of the GPL 2.
 *
 *
 * Framebuffer support for device namespace:
 *
 * Isolate framebuffer state between device namespaces, and multiplex access
 * to hardware state between the virtual framebuffers.
 *
 * Associate with each device namespace a shadow, virtual 'struct fb_info'
 * (vinfo) and a backing buffer, for each registered hardware framebuffer.
 * Access from an active (foreground) device namespace is passed-through
 * directly to the underlying device. Access from a passive (background)
 * device namespace is directed to the virtal fb_info.
 *
 * The backing buffer is used for mmap-ed memory in passive namespace, so
 * any drawing occurs to the backing buffer instead of to the real driver.
 * When the active namespace changes, perform a context-switch and swap
 * such mappings: remap memory previously in foreground to the backing
 * buffer, and from background to the underlying device. The contents of
 * the framebuffer is saved and restored in (from) the bacgkround (active)
 * backing buffer, respectively.
 *
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 * The full GPL 2 License is included in this distribution in the file called
 * COPYING
 *
 * Cellrox can be contacted at oss@cellrox.com
 */

#ifdef CONFIG_DEV_NS
#define DEBUG
#define FB_DEV_NS_DEBUG_NOISE

#ifdef FB_DEV_NS_DEBUG
#define pr_fmt(fmt) \
	"[%d] devns:framebuffer [%s:%d]: " fmt, \
	current->pid, __func__, __LINE__
#else
#define pr_fmt(fmt) \
	"[%d] devns:framebuffer: " fmt, current->pid
#endif

#endif /* CONFIG_DEV_NS */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/pagemap.h>
#include <linux/fb.h>
#include <linux/fb_devns.h>
#include <linux/dev_namespace.h>


static DEFINE_MUTEX(fb_ns_mutex);

struct fb_inode {
	struct inode *inode;
	int count;
};

struct fb_colreg {
	__u32 regno;
	__u16 red;
	__u16 green;
	__u16 blue;
	__u16 transp;
};

struct fb_ns_info {
	struct fb_dev_ns *fb_ns;
	struct fb_info *info;

	struct fb_var_screeninfo var;

	struct fb_colreg *colreg;
	int colreg_pos;
	int colreg_len;

	void *vmem_buf;
	size_t vmem_len;
#ifdef CONFIG_FB_DEV_NS_DEBUG
	void *screen_base;
	unsigned long screen_size;
#endif

	struct fb_inode *inodes;
	int inodes_len;
};

struct fb_dev_ns {
	struct fb_info *fb[FB_MAX];
	struct dev_ns_info dev_ns_info;
};

/* fb_ns_id, get_fb_ns(), get_fb_ns_cur(), put_fb_ns() */
DEFINE_DEV_NS_INFO(fb)

/**********************************************************************/

static inline bool fb_info_is_virt(struct fb_info *fb_info)
{
	return fb_info->flags & FBINFO_DEV_NS;
}

/**********************************************************************/

#ifdef FB_DEV_NS_DEBUG_NOISE
#define FB_NOISE(...)  pr_debug(__VA_ARGS__)
#else
#define FB_NOISE(...)  do { /* */ } while (0)
#endif

#ifdef FB_DEV_NS_DEBUG
#define fb_debug_info(info)  \
	_fb_debug_info(info, __func__, __stringify(__LINE__))
#define fb_debug_diff(info)  \
	_fb_debug_diff(info, __func__, __stringify(__LINE__))
#else
#define fb_debug_info(info)  \
	_fb_debug_info(info, "", "")
#define fb_debug_diff(info)  \
	_fb_debug_diff(info, "", "")
#endif

static void _fb_debug_info(struct fb_info *info,
			   const char *func, const char *line)
{
	printk(KERN_DEBUG "[%d] devns:framebuffer [%s:%s]:\n"
		 "  |-> info 0x%p node %d (%s)\n"
		 "  |-> info smem_start 0x%lx smem_len 0x%x\n"
		 "  |-> info screen base 0x%p screen_size 0x%lx\n",
		 current->pid, func ? : "", line ? : "",
		 info, info->node, fb_info_is_virt(info) ? "virt" : "info",
		 info->fix.smem_start, info->fix.smem_len,
		 info->screen_base, info->screen_size);
}

static void _fb_debug_diff(struct fb_info *virt,
			     const char *func, const char *line)
{
	struct fb_ns_info *fb_ns_info;
	struct fb_info *info;

	BUG_ON(!fb_info_is_virt(virt));

	fb_ns_info = virt->par;
	info = fb_virt_to_info(virt);

#define FB_DEBUG_DIFF(field, format, func, line) \
	do {  \
		if (virt->field != info->field)  \
			printk(KERN_DEBUG "[%d] devns:framebuffer [%s:%s]: "  \
			       "*** fb_info diff *** " # field  \
			       " virt " format " info " format " (0x%p)\n",  \
			       current->pid, func ? : "", line ? : "",  \
			       virt->field, info->field, info);  \
	} while (0)

	FB_DEBUG_DIFF(fix.smem_len, "0x%d", func, line);
	FB_DEBUG_DIFF(screen_size, "0x%lx", func, line);

#define FB_DEBUG_DIFF_2(field1, format1, field2, format2, func, line) \
	do {  \
		if ((unsigned long) field1 != (unsigned long) field2)  \
			printk(KERN_DEBUG "[%d] devns:framebuffer [%s:%s]: "  \
			       "*** fb_info diff *** "  \
			       # field1 " " format1 " != "  \
			       # field2 " " format2 " (0x%p)\n",  \
			       current->pid, func ? : "", line ? : "",  \
			       field1, field2, info);  \
	} while (0)


#ifdef CONFIG_FB_DEV_NS_DEBUG
	FB_DEBUG_DIFF_2(fb_ns_info->screen_base, "0x%p",
			info->screen_base, "0x%p", func, line);
	FB_DEBUG_DIFF_2(fb_ns_info->screen_size, "0x%lx",
			info->screen_size, "0x%lx", func, line);
	BUG_ON(fb_ns_info->screen_base != info->screen_base ||
	       fb_ns_info->screen_size != info->screen_size);
#endif
	FB_DEBUG_DIFF_2(fb_ns_info->vmem_len, "0x%zx",
			info->fix.smem_len, "0x%x", func, line);

	WARN_ON(fb_ns_info->vmem_len != info->fix.smem_len);
}


/**********************************************************************/

static struct notifier_block fb_ns_switch_notifier;

static struct dev_ns_info *fb_devns_create(struct dev_namespace *dev_ns)
{
	struct fb_dev_ns *fb_ns;
	struct dev_ns_info *dev_ns_info;

	fb_ns = kzalloc(sizeof(*fb_ns), GFP_KERNEL);
	if (!fb_ns)
		return ERR_PTR(-ENOMEM);

	pr_info("new fb_dev_ns 0x%p (d 0x%p)\n", fb_ns, dev_ns);

	dev_ns_info = &fb_ns->dev_ns_info;

	dev_ns_info->nb = fb_ns_switch_notifier;
	dev_ns_register_notify(dev_ns, &dev_ns_info->nb);

	return dev_ns_info;
}

static void fb_devns_release(struct dev_ns_info *dev_ns_info)
{
	struct fb_dev_ns *fb_ns;

	fb_ns = container_of(dev_ns_info, struct fb_dev_ns, dev_ns_info);

	pr_info("del fb_dev_ns 0x%p (d 0x%p)\n", fb_ns, dev_ns_info->dev_ns);
	dev_ns_unregister_notify(dev_ns_info->dev_ns, &dev_ns_info->nb);

	kfree(fb_ns);
}

static struct dev_ns_ops fb_ns_ops = {
	.create = fb_devns_create,
	.release = fb_devns_release,
};

/**********************************************************************/

/* Given a virtual fb, indicate if it's active (foreground) */
static bool fb_virt_is_active(struct fb_info *virt)
{
	struct fb_ns_info *fb_ns_info;

	BUG_ON(!fb_info_is_virt(virt));

	fb_ns_info = virt->par;
	return is_active_dev_ns(fb_ns_info->fb_ns->dev_ns_info.dev_ns);
}

/* Given a fb, convert from virt info to HW info (if necessary) */
struct fb_info *fb_virt_to_info(struct fb_info *virt)
{
	struct fb_info *info;
	struct fb_ns_info *fb_ns_info;

	if (fb_info_is_virt(virt)) {
		fb_ns_info = virt->par;
		info = fb_ns_info->info;

		/*
		 * TODO: add debug divergence checks between virt & info:
		 * matching screen_size, fix contents, etc.
		 */
	} else
		info = virt;

	return info;
}

/* Given a fb, convert from virt info to HW info if active (foreground) */
struct fb_info *fb_virt_to_info_ns(struct fb_info *virt)
{
	if (fb_info_is_virt(virt) && fb_virt_is_active(virt))
		return fb_virt_to_info(virt);
	else
		return virt;
}

/*
 * Track inodes pointing to the device, to easily find whoever mmaps it.
 * (unlikely to have more than one or a few inodes)
 */

struct fb_inode *find_fb_inode(struct fb_info *virt, struct inode *inode)
{
	struct fb_ns_info *fb_ns_info = virt->par;
	struct fb_inode *inodes;
	int i;

	inodes = fb_ns_info->inodes;

	for (i = 0; i < fb_ns_info->inodes_len; i++) {
		if (inodes[i].inode == inode)
			return &inodes[i];
	}

	return NULL;
}

int track_fb_inode(struct fb_info *virt, struct inode *inode)
{
	struct fb_ns_info *fb_ns_info = virt->par;
	struct fb_inode *fb_inode;

	mutex_lock(&fb_ns_mutex);
	fb_inode = find_fb_inode(virt, inode);
	if (!fb_inode) {
		int len = fb_ns_info->inodes_len;

		pr_debug("fb_inode alloc new len %d\n", len + 1);
		fb_inode = krealloc(fb_ns_info->inodes,
				    (len + 1) * sizeof(*fb_inode),
				    GFP_KERNEL);
		if (!fb_inode) {
			mutex_unlock(&fb_ns_mutex);
			return -ENOMEM;
		}

		fb_ns_info->inodes = fb_inode;
		fb_ns_info->inodes_len = len + 1;

		fb_inode = &fb_inode[len];
		fb_inode->inode = inode;
		fb_inode->count = 0;
	}
	fb_inode->count++;
	pr_debug("fb_inode track 0x%p (idx %d)\n", inode, virt->node);
	mutex_unlock(&fb_ns_mutex);

	return 0;
}

void untrack_fb_inode(struct fb_info *virt, struct inode *inode)
{
	struct fb_ns_info *fb_ns_info = virt->par;
	struct fb_inode *fb_inode;

	mutex_lock(&fb_ns_mutex);
	fb_inode = find_fb_inode(virt, inode);
	BUG_ON(!fb_inode);

	pr_debug("fb_inode untrack 0x%p (idx %d)\n", inode, virt->node);
	if (fb_inode && --fb_inode->count == 0) {
		int len = fb_ns_info->inodes_len - 1;

		*fb_inode = fb_ns_info->inodes[len];
		fb_ns_info->inodes_len = len;
		fb_ns_info->inodes = krealloc(fb_ns_info->inodes,
					      len, GFP_KERNEL);
		pr_debug("fb_inode drop len %d\n", len);
	}
	mutex_unlock(&fb_ns_mutex);
}


static struct fb_ops fb_devns_ops;

/*
 * Duplicate the fb_ops for the virtual fb_info, substituting certain
 * methods with those that should be used when running in background.
 * This is needed to maintain compatibility with original call-paths,
 * regardless of whether the underlying device has or has not defined
 * said methods.
 */
static struct fb_ops *fb_ns_make_fb_ops(struct fb_info *info)
{
	struct fb_ops *fbops;

	fbops = kmalloc(sizeof(*fbops), GFP_KERNEL);
	if (fbops == NULL)
		return NULL;

	memcpy(fbops, &fb_devns_ops, sizeof(*fbops));

	if (!info->fbops->fb_check_var)
		fbops->fb_check_var = NULL;
	if (!info->fbops->fb_setcmap)
		fbops->fb_setcmap = NULL;

	/* TODO: almost always used from console... add sanity
	 * that complains if called?  fb_rotate never called */
	if (!info->fbops->fb_fillrect)
		fbops->fb_fillrect = NULL;
	if (!info->fbops->fb_copyarea)
		fbops->fb_copyarea = NULL;
	if (!info->fbops->fb_imageblit)
		fbops->fb_imageblit = NULL;
	if (!info->fbops->fb_cursor)
		fbops->fb_cursor = NULL;
	if (!info->fbops->fb_rotate)
		fbops->fb_rotate = NULL;

	if (!info->fbops->fb_ioctl)
		fbops->fb_ioctl = NULL;
	if (!info->fbops->fb_get_caps)
		fbops->fb_get_caps = NULL;
	if (!info->fbops->fb_compat_ioctl)
		fbops->fb_compat_ioctl = NULL;

	/* TODO: almost always used from console... add sanity
	 * that complains if called?  */
	if (!info->fbops->fb_debug_enter)
		fbops->fb_debug_enter = NULL;
	if (!info->fbops->fb_debug_leave)
		fbops->fb_debug_enter = NULL;

	fbops->owner = info->fbops->owner;

	return fbops;
}

static void fb_ns_free_fb_ops(struct fb_ops *fbops)
{
	kfree(fbops);
}

static struct fb_info *fb_ns_info_alloc(struct fb_dev_ns *fb_ns,
					struct fb_info *fb_info)
{
	struct fb_info *fb_virt;
	struct fb_ns_info *fb_ns_info;
	void *vmem = NULL;
	size_t vlen;

	fb_virt = framebuffer_alloc(sizeof(struct fb_ns_info), fb_info->device);
	if (fb_virt == NULL)
		return NULL;

	pr_info("new fb_virt 0x%p for fb_info 0x%p (%s) idx %d (d 0x%p)\n",
		fb_virt, fb_info, fb_info->fix.id,
		fb_info->node, fb_ns->dev_ns_info.dev_ns);

	/* copy main pieces from underlying fb_info */
	fb_virt->node = fb_info->node;
	fb_virt->flags = fb_info->flags;
	atomic_set(&fb_virt->count, 0);
	mutex_init(&fb_virt->lock);
	mutex_init(&fb_virt->mm_lock);

	strncpy(fb_virt->fix.id, fb_info->fix.id, sizeof(fb_virt->fix.id));

	fb_virt->pseudo_palette = fb_info->pseudo_palette;
	fb_virt->cmap = fb_info->cmap;
	fb_virt->pixmap = fb_info->pixmap;
	fb_virt->fix = fb_info->fix;
	fb_virt->var = fb_info->var;

	/* allocate per-namespace virtual buffer */
	vlen = fb_info->fix.smem_len;
	if (vlen) {
		vmem = vmalloc_user(vlen);  /* already zeroed out */
		if (vmem == NULL) {
			framebuffer_release(fb_virt);
			return NULL;
		}
	}

	fb_debug_info(fb_info);
	fb_debug_info(fb_virt);

	/* setup namespace aware data */
	fb_ns_info = fb_virt->par;

	fb_ns_info->vmem_len = vlen;
	fb_ns_info->vmem_buf = vmem;

#ifdef CONFIG_FB_DEV_NS_DEBUG
	fb_ns_info->screen_base = fb_info->screen_base;
	fb_ns_info->screen_size = fb_info->screen_size;
#endif

	fb_ns_info->colreg_pos = 0;
	fb_ns_info->colreg_len = 0;

	fb_ns_info->fb_ns = fb_ns;
	fb_ns_info->info = fb_info;

	/* make it namespace aware */
	fb_virt->flags |= FBINFO_DEV_NS;
	fb_virt->fbops = fb_ns_make_fb_ops(fb_info);
	if (fb_virt->fbops == NULL) {
		vfree(fb_ns_info->vmem_buf);
		framebuffer_release(fb_virt);
		return NULL;
	}

	fb_virt->screen_base = vmem;
	fb_virt->screen_size = fb_info->screen_size;
	fb_virt->fix.smem_start = (unsigned long) vmem;
	fb_virt->fix.smem_len = vlen;

	fb_debug_info(fb_info);
	fb_debug_info(fb_virt);

	return fb_virt;
}

static void fb_ns_info_free(struct fb_info *fb_virt)
{
	struct fb_ns_info *fb_ns_info;

	fb_ns_info = fb_virt->par;

	pr_info("del fb_virt 0x%p for fb_info 0x%p idx %d\n",
		  fb_virt, fb_ns_info->info, fb_virt->node);

	vfree(fb_ns_info->vmem_buf);
	kfree(fb_ns_info->colreg);
	fb_ns_free_fb_ops(fb_virt->fbops);
	framebuffer_release(fb_virt);
}

/*
 * Return the namespace-aware fb_info for a given fb_info.
 * From now onward, all access is via the namespace-aware fb_info,
 * including the cleanup via the callback fb_ns_destroy().
 */
struct fb_info *get_fb_info_ns(struct fb_info *fb_info)
{
	struct fb_dev_ns *fb_ns;
	struct fb_info *fb_virt;
	int idx;

	fb_ns = get_fb_ns_cur();
	if (!fb_ns)
		return NULL;

	idx = fb_info->node;

	pr_debug("get fb_info 0x%p idx %d (d 0x%p)\n",
		 fb_info, fb_info->node, fb_ns->dev_ns_info.dev_ns);

	mutex_lock(&fb_ns_mutex);
	if (fb_ns->fb[idx] == NULL)
		fb_ns->fb[idx] = fb_ns_info_alloc(fb_ns, fb_info);

	fb_virt = fb_ns->fb[idx];

	if (fb_virt)
		atomic_inc(&fb_virt->count);
	else
		fb_virt = NULL;
	mutex_unlock(&fb_ns_mutex);

	pr_debug("got fb_virt 0x%p idx %d (d 0x%p)\n",
		 fb_virt, fb_info->node, fb_ns->dev_ns_info.dev_ns);

	if (fb_virt == NULL)
		put_fb_ns(fb_ns);

	return fb_virt;
}

void put_fb_info_ns(struct fb_info *fb_virt)
{
	struct fb_ns_info *fb_ns_info;

	if (fb_virt == NULL)
		return;

	pr_debug("put fb_info 0x%p idx %d\n", fb_virt, fb_virt->node);

	fb_ns_info = fb_virt->par;
	put_fb_ns(fb_ns_info->fb_ns);

	if (atomic_dec_and_test(&fb_virt->count)) {
		/*
		 * Note: it is safe to do this without fb_ns_mutex
		 * because we reach here only once per fb_ns_info.
		 */
		fb_ns_info->fb_ns->fb[fb_virt->node] = NULL;
		fb_ns_info_free(fb_virt);
	}
}

/**********************************************************************/

/* for fb_xxxx operations that should never occur */
static int fb_ns_open(struct fb_info *virt, int user)  { BUG(); }
static int fb_ns_release(struct fb_info *virt, int user)  { BUG(); }
static void fb_ns_destroy(struct fb_info *virt)  { BUG(); }

static int fb_ns_check_var(struct fb_var_screeninfo *var, struct fb_info *virt)
{
	struct fb_info *info;
	int ret = 0;

	BUG_ON(!fb_info_is_virt(virt));

	/* TODO: maybe do check against current HW par? */

	fb_debug_info(virt);
	fb_debug_diff(virt);

	info = fb_virt_to_info(virt);
	if (info->fbops->fb_check_var)
		ret = info->fbops->fb_check_var(var, info);

	return ret;
}

static int fb_ns_set_par(struct fb_info *virt)
{
	struct fb_ns_info *fb_ns_info = virt->par;

	BUG_ON(!fb_info_is_virt(virt));

	/* TODO: check var in FB_ACTIVATE_TEST against real info? */
	/* TODO: save certain operations on var for later? */

	fb_debug_info(virt);
	fb_debug_diff(virt);

	/* stash parameters for later, when fb becomes active */
	fb_ns_info->var = virt->var;

	return 0;
}

static int fb_ns_setcolreg(unsigned regno, unsigned red, unsigned green,
			   unsigned blue, unsigned transp, struct fb_info *virt)
{
	struct fb_ns_info *fb_ns_info;
	struct fb_colreg *colreg;

	BUG_ON(!fb_info_is_virt(virt));

	/* TODO: can search and replace existing regno, if exists */

	fb_debug_info(virt);
	fb_debug_diff(virt);

	/*
	 * The following is protected from races because the framebuffer
	 * system calls fb_setcmap()->fb_setcolreg() with lock_fb_info()
	 */
	fb_ns_info = virt->par;
	colreg = fb_ns_info->colreg;

	if (fb_ns_info->colreg_pos == fb_ns_info->colreg_len) {
		int size = fb_ns_info->colreg_pos + 256;
		colreg = krealloc(colreg, size * sizeof(*colreg), GFP_KERNEL);
		if (colreg == NULL)
			return -ENOMEM;
		fb_ns_info->colreg = colreg;
		fb_ns_info->colreg_len = size;
	}

	colreg = &colreg[fb_ns_info->colreg_pos++];

	colreg->regno = regno;
	colreg->red = red;
	colreg->green = green;
	colreg->blue = blue;
	colreg->transp = transp;

	FB_NOISE("fb_info 0x%p idx %d COLREG regno %d, pos %d\n",
		 virt, virt->node, regno, fb_ns_info->colreg_pos);

	return 0;
}

static int fb_ns_setcmap(struct fb_cmap *cmap, struct fb_info *virt)
{
	BUG_ON(!fb_info_is_virt(virt));

	/* TODO: this can be device specific, may need more work? */
	/*
	 * in particular, HW can keep its own map, and we here override
	 * an entry. if the other namespace does/expects the same value
	 * all is well, but if each namespace to its own - we need to
	 * save these values and context-switch (HW) on namespace swtich.
	 */

	fb_debug_info(virt);
	fb_debug_diff(virt);

	FB_NOISE("fb_info 0x%p idx %d CMAP start %d, len %d\n",
		 virt, virt->node, cmap->start, cmap->len);

	/* TODO: see also fbmem.c:fb_set_cmap() */

	return 0;
}

static int fb_ns_cursor(struct fb_info *virt, struct fb_cursor *cursor)
{
	BUG_ON(!fb_info_is_virt(virt));

	pr_info("unexpected fb_cursor() on virt fb_info 0x%p idx %d\n",
		virt, virt->node);

	return 0;
}

static void fb_ns_rotate(struct fb_info *virt, int angle)
{
	pr_info("unexpected fb_rotate() on virt fb_info 0x%p idx %d\n",
		virt, virt->node);
}

static int fb_ns_ioctl(struct fb_info *virt, unsigned int cmd,
		       unsigned long arg)
{
	BUG_ON(!fb_info_is_virt(virt));

	fb_debug_diff(virt);

	pr_info("specialized fb_ioctl() on virt fb_info 0x%p idx %d\n",
		virt, virt->node);
	pr_info(" |-> ioctl cmd %d, arg %ld/0x%lx\n", cmd, arg, arg);

	return 0;
}

static int fb_ns_compat_ioctl(struct fb_info *virt, unsigned cmd,
			      unsigned long arg)
{
	BUG_ON(!fb_info_is_virt(virt));

	fb_debug_diff(virt);

	pr_info("specialized fb_compat_ioctl() on virt fb_info 0x%p idx %d\n",
		virt, virt->node);
	pr_info(" |-> ioctl cmd %d, arg %ld/0x%lx\n", cmd, arg, arg);

	return 0;
}

static int fb_ns_mmap(struct fb_info *virt, struct vm_area_struct *vma)
{
	void *addr;

	BUG_ON(!fb_info_is_virt(virt));

	fb_debug_info(virt);
	fb_debug_diff(virt);

	addr = (void *) virt->fix.smem_start;

	pr_info("fb_mmap fb_info 0x%p idx %d\n", virt, virt->node);
	pr_info(" |-> addr 0x%p vm_start 0x%lx vm_size 0x%lx vm_pgoff 0x%lx\n",
		addr, vma->vm_start, vma->vm_end - vma->vm_start, vma->vm_pgoff);

	return remap_vmalloc_range(vma, addr, vma->vm_pgoff);
}

static void fb_ns_get_caps(struct fb_info *virt, struct fb_blit_caps *caps,
			   struct fb_var_screeninfo *var)
{
	struct fb_info *info;

	/* TODO: may need to copy HW's var temporarily for this: e.g.
	 * for s3fb.c which answers based on its var->bits_per_pixel
	 */

	BUG_ON(!fb_info_is_virt(virt));

	fb_debug_diff(virt);

	info = fb_virt_to_info(virt);
	info->fbops->fb_get_caps(info, caps, var);
}

static int fb_ns_debug_enter(struct fb_info *virt)
{
	struct fb_info *info;

	/* resort to whatever underlying HW logic */

	BUG_ON(!fb_info_is_virt(virt));

	pr_info("unexpected fb_debug_enter() on virt fb_info 0x%p idx %d\n",
		virt, virt->node);

	info = fb_virt_to_info(virt);
	return info->fbops->fb_debug_enter(info);
}

static int fb_ns_debug_leave(struct fb_info *virt)
{
	struct fb_info *info;

	/* resort to whatever underlying HW logic */

	BUG_ON(!fb_info_is_virt(virt));

	pr_info("unexpected fb_debug_leave() on virt fb_info 0x%p idx %d\n",
		virt, virt->node);

	info = fb_virt_to_info(virt);
	return info->fbops->fb_debug_leave(info);
}

static struct fb_ops fb_devns_ops = {
	.fb_open		= fb_ns_open,
	.fb_release		= fb_ns_release,
	.fb_read		= fb_sys_read,
	.fb_write		= fb_sys_write,
	.fb_check_var		= fb_ns_check_var,
	.fb_set_par		= fb_ns_set_par,
	.fb_setcolreg		= fb_ns_setcolreg,
	.fb_setcmap		= fb_ns_setcmap,
	.fb_blank		= NULL,  /* keep default action */
	.fb_pan_display		= NULL,  /* keep default action */
	.fb_fillrect		= cfb_fillrect,  /* like vfb */
	.fb_copyarea		= cfb_copyarea,  /* like vfb */
	.fb_imageblit		= cfb_imageblit,  /* like vfb */
	.fb_cursor		= fb_ns_cursor,
	.fb_rotate		= fb_ns_rotate,
	.fb_sync		= NULL,  /* no virtual sync */
	.fb_ioctl		= fb_ns_ioctl,
	.fb_compat_ioctl	= fb_ns_compat_ioctl,
	.fb_mmap		= fb_ns_mmap,
	.fb_get_caps		= fb_ns_get_caps,
	.fb_destroy		= fb_ns_destroy,
	.fb_debug_enter		= fb_ns_debug_enter,
	.fb_debug_leave		= fb_ns_debug_leave,
};

/**********************************************************************/

static int fb_ns_apply_colreg(struct fb_info *virt)
{
	struct fb_ns_info *fb_ns_info;
	struct fb_colreg *colreg;
	struct fb_info *info;
	int i, ret, err = 0;

	fb_ns_info = virt->par;
	info = fb_virt_to_info(virt);

	/*
	 * fb_ns_setcolreg() is protected by framebuffer system using
	 * lock_fb_info() - use same lock here to protect colreg data.
	 */
	if (!lock_fb_info(virt))
		BUG();

	colreg = fb_ns_info->colreg;
	i = fb_ns_info->colreg_pos;

	pr_debug("virt 0x%p pending colregs %d\n", virt, i);

	while (i--) {
		ret = info->fbops->fb_setcolreg(colreg->regno, colreg->red,
						colreg->green, colreg->blue,
						colreg->transp, info);
		if (ret < 0)
			pr_info("fb_info 0x%p colreg err %d/%d, regno %d\n",
				virt, ret, err, colreg->regno);
		if (ret < 0 && !err)
			err = ret;
		colreg++;
	}

	fb_ns_info->colreg_pos = 0;

	unlock_fb_info(virt);
	return err;
}

static int fb_ns_apply_set_par(struct fb_info *virt)
{
	struct fb_ns_info *fb_ns_info;
	struct fb_var_screeninfo *var;
	struct fb_info *info;
	int ret = 0;

	fb_ns_info = virt->par;
	info = fb_virt_to_info(virt);

	/*
	 * fb_ns_set_par() is protected by framebuffer system using
	 * lock_fb_info() - use same lock here to protect var/par data.
	 */
	if (!lock_fb_info(virt))
		BUG();

	var = &fb_ns_info->var;

	/* if xres == 0, then there was no pending fb_set_par */
	if (var->xres == 0)
		goto out;

	info->var = *var;
	ret = info->fbops->fb_set_par(info);

	if (ret < 0)
		pr_info("fb_info 0x%p setpar err %d\n", virt, ret);

	var->xres = 0;

 out:
	unlock_fb_info(virt);
	return ret;
}

/*
 * Save current hardware buffer in the virtual buffer of @prev,
 * and restore the contents saved in the virtual buffer of @next.
 */
static void fb_ns_swap_vmem(struct fb_info *virt, int activate)
{
	struct fb_info *info;
	size_t size;

	info = fb_virt_to_info(virt);
	size = virt->fix.smem_len;

	fb_debug_info(virt);
	fb_debug_diff(virt);

	pr_info("fb_info 0x%p idx %d copy %s virtual buffer (size 0x%zx)\n",
		info, info->node, activate ? "from" : "to", size);

	/* follow logic in fb_read()/fb_write() */
	if (info->fbops->fb_sync)
		info->fbops->fb_sync(info);

	pr_info("fb_info 0x%p screen 0x%p, virt 0x%p virt->screen 0x%p\n",
		info, info->screen_base, virt, virt->screen_base);

	if (activate)
		fb_memcpy_tofb(info->screen_base, virt->screen_base, size);
	else
		fb_memcpy_fromfb(virt->screen_base, info->screen_base, size);
}

struct fb_mmlist {
	struct mm_struct *mm;
	unsigned long start;
	unsigned long size;
	struct fb_mmlist *next;
};

static int do_fb_ns_remap(struct fb_info *virt, struct address_space *mapping)
{
	struct vm_area_struct *vma;
	//struct prio_tree_iter iter;
	struct fb_mmlist *mmhead = NULL;
	struct fb_mmlist *mmlist;
	int ret, err = 0;

	/* find mm_structs that have hardware/virtual buffer to remap */
	mutex_lock(&mapping->i_mmap_mutex);
	vma_interval_tree_foreach(vma, &mapping->i_mmap, 0, ULONG_MAX) {
		mmlist = kmalloc(sizeof(*mmlist), GFP_KERNEL);
		if (!mmlist) {
			err = -ENOMEM;
			break;
		}

		mmlist->mm = vma->vm_mm;
		mmlist->start = vma->vm_start;
		mmlist->size = vma->vm_end - vma->vm_start;
		atomic_inc(&vma->vm_mm->mm_count);

		mmlist->next = mmhead;
		mmhead = mmlist;

		pr_debug("  |-> collect mm 0x%p (0x%lx, 0x%lx)\n",
			 vma->vm_mm, mmlist->start, mmlist->size);
	}
	mutex_unlock(&mapping->i_mmap_mutex);

	if (err < 0)
		goto out;

	/* loop over found mm_structs: unmap old buffer, map new one */
	for (mmlist = mmhead; mmlist; mmlist = mmlist->next) {
		struct mm_struct *mm = mmlist->mm;
		unsigned long start = mmlist->start;
		unsigned long size = mmlist->size;

		down_write(&mm->mmap_sem);

		vma = find_vma(mm, mmlist->start);
		BUG_ON(vma->vm_file->f_mapping != mapping);

		if (vma) {
			pr_debug("  |-> vma (0x%lx 0x%lx 0x%lx) inode 0x%p\n",
				 vma->vm_start, vma->vm_end - vma->vm_start,
				 vma->vm_pgoff,
				 vma->vm_file->f_dentry->d_inode);
			pr_debug("  |-> remap mm 0x%p (0x%lx 0x%lx)\n",
				 vma->vm_mm, mmlist->start, mmlist->size);

			/* remove previous mapping */
			zap_page_range(vma, start, size, NULL);

			ret = vma_adjust(vma, vma->vm_start, vma->vm_end,
					 0, NULL);
			if (ret < 0)
				pr_info("  |-> vma adjust %d\n", ret);

			/*
			 * This may not be IO memory now, but may have been.
			 * If active, it will be re-set by underlying mmap.
			 */
			vma->vm_flags &= ~VM_IO;

			/*
			 * Create new mapping. (Note: the native mmap will
			 * eventually call the fb_mmap of info (if active)
			 * or virt (if inactive).
			 */
			ret = vma->vm_file->f_op->mmap(vma->vm_file, vma);
			if (ret < 0)
				pr_info("  |-> remap mm error %d\n", ret);

			/* proceed in case of errors */
			if (ret < 0 && !err)
				err = ret;
		}

		up_write(&mm->mmap_sem);
	}

out:
	while (mmhead) {
		mmlist = mmhead;
		mmhead = mmhead->next;
		mmdrop(mmlist->mm);
		kfree(mmlist);
	}

	return err;
}

static int fb_ns_swap_mmap(struct fb_info *virt, bool activate)
{
	struct fb_ns_info *fb_ns_info = virt->par;
	struct inode *inode;
	int i, ret, err = 0;

	/* iterate through tracked inodes to locate users */

	for (i = 0; i < fb_ns_info->inodes_len; i++) {
		inode = fb_ns_info->inodes[i].inode;
		pr_debug("tracked inode 0x%p fb_info 0x%p idx %d\n",
			 inode, virt, virt->node);
		ret = do_fb_ns_remap(virt, inode->i_mapping);
		if (ret < 0 || !err)
			err = ret;
	}

	return err;
}

/*
 * Switch active (foreground) framebuffer:
 * - Swap mapping of framebuffer memory in switched namespaces.
 * - Save framebuffer memory in backing buffer of previous active namespace,
 *   and restore from backing buffer of the newly active namespace.
 * - Copy (pending) state from the virtual fb_info to the underlying device
 *
 * Save/restore of the framebuffer memory may not be strictly necessary if
 * the new foreground completely redraws the screen; However, it may provide
 * a (visually) smoother transition if the redraw doesn't occur promptly.
 *
 * By default it is expected that after being switched to foreground mode,
 * userspace will redraw the screen. (see below why).
 */

static int do_fb_activate_ns(struct fb_info *virt, bool activate)
{
	int ret;

	pr_info("%sactivate fb_info 0x%p idx %d\n",
		activate ? "" : "de", virt, virt->node);

	ret = fb_ns_swap_mmap(virt, activate);
	if (ret < 0)
		return ret;

	fb_ns_swap_vmem(virt, activate);

	if (activate) {
		(void) fb_ns_apply_colreg(virt);
		(void) fb_ns_apply_set_par(virt);
	}

#ifdef CONFIG_FB_DEV_NS_PAN
	/*
	 * Force-update the display by calling fb_pan_display() directly.
	 *
	 * Usually, the plain memory copy in fb_ns_swap_vmem() suffices.
	 * But if not (e.g., on x86-KVM emulator), this re-enforces the
	 * restored display contents.
	 *
	 * (Caution: in the prehistory, this could cause occasional 1/2
	 * second hiccups, if fb_pan_display() occured when another was
	 * already in progress - because fb drivers usually don't handle
	 * two irq callbacks and will timeout waiting for display vsync).
	 */
	if (activate) {
		struct fb_info *info = fb_virt_to_info(virt);
		/*
		 * lock both virt an info: protect against both a change
		 * to virt->var and a concurrent info->fb_pan_dipslay()
		 */
		(void) lock_fb_info(virt);  /* can't fail */
		(void) lock_fb_info(info);  /* can't fail */

		fb_pan_display(info, &virt->var);

		unlock_fb_info(info);
		unlock_fb_info(virt);
	}
#endif

	return 0;
}

/* dev_ns and resepctive fb_dev_ns protected (by caller) */
static int fb_activate_ns(struct dev_namespace *dev_ns, bool activate)
{
	struct fb_dev_ns *fb_ns;
	struct fb_info *virt;
	int ret, err = 0;
	int i;

	mutex_lock(&fb_ns_mutex);

	pr_info("  |-> %sactivate devns 0x%p (%s)\n",
		activate ? "" : "de", dev_ns, dev_ns->tag);

	/* while in switch callback, @dev_ns and @fb_dev_ns are protectd */
	fb_ns = find_fb_ns(dev_ns);
	WARN(fb_ns == NULL, "devns 0x%p: no matching fb_ns\n", dev_ns);

	for (i = 0; i < FB_MAX; i++) {
		virt = fb_ns->fb[i];
		if (!virt)
			continue;
		ret = do_fb_activate_ns(virt, activate);
		if (ret < 0)
			err = ret;
	}

	mutex_unlock(&fb_ns_mutex);
	return err;
}

/* dev_ns and resepctive fb_dev_ns protected by caller */
static int fb_ns_switch_callback(struct notifier_block *self,
				 unsigned long action, void *data)
{
	struct dev_namespace *dev_ns = data;
	int ret = 0;

	switch (action) {
	case DEV_NS_EVENT_ACTIVATE:
		pr_info("switch to devns 0x%p (%s)\n", dev_ns, dev_ns->tag);
		ret = fb_activate_ns(dev_ns, true);
		break;
	case DEV_NS_EVENT_DEACTIVATE:
		pr_info("switch from devns 0x%p (%s)\n", dev_ns, dev_ns->tag);
		ret = fb_activate_ns(dev_ns, false);
		break;
	}

	return ret;
}

static struct notifier_block fb_ns_switch_notifier = {
	.notifier_call = fb_ns_switch_callback,
};

/*
 * Get notifications about framebuffer register/unregister, so that
 * we can properly deactivate a namespace-aware framebuffer.
 */
static int fb_ns_event_callback(struct notifier_block *self,
				unsigned long action, void *data)
{
	struct fb_event *event = data;
	struct fb_info *info;
	struct fb_info *virt;
	int ret = 0;

	if (!event || !event->info)
		return 0;

	info = event->info;

	pr_debug("FB_EVENT %lu fb_info 0x%p idx %d\n",
		 action, info, info->node);
	fb_debug_info(info);

	switch (action) {
	case FB_EVENT_FB_REGISTERED:
		/* TODO: debug info */
		break;
	case FB_EVENT_FB_UNREGISTERED:
		pr_debug("FB_UNREGISTERED 0x%p idx %d\n", info, info->node);
		fb_debug_info(info);

		/* TODO: can unregister come from "dead" FB? */

		virt = get_fb_info_ns(info);
		if (fb_virt_is_active(virt))
			(void) do_fb_activate_ns(virt, false);
		put_fb_info_ns(virt);
		break;
	}

	return ret;
}

static struct notifier_block fb_ns_event_notifier = {
	.notifier_call = fb_ns_event_callback,
};

static int __init fb_init(void)
{
	int ret;

	ret = fb_register_client(&fb_ns_event_notifier);
	if (ret < 0)
		goto err;
	ret = DEV_NS_REGISTER(fb, "framebuffer");
	if (ret < 0)
		goto err_fb;
	return 0;

 err_fb:
	fb_unregister_client(&fb_ns_event_notifier);
 err:
	return ret;
}

static void __exit fb_exit(void)
{
	DEV_NS_UNREGISTER(fb);
	fb_unregister_client(&fb_ns_event_notifier);
}

module_init(fb_init);
module_exit(fb_exit);

