/**
 * collectd - src/plugin.c
 * Copyright (C) 2005  Florian octo Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at verplant.org>
 **/

#include "collectd.h"

#include <ltdl.h>

#include "plugin.h"
#include "multicast.h"

typedef struct plugin
{
	char *type;
	void (*init) (void);
	void (*read) (void);
	void (*write) (char *host, char *inst, char *val);
	struct plugin *next;
} plugin_t;

static plugin_t *first_plugin = NULL;

#ifdef HAVE_LIBRRD
extern int operating_mode;
#endif

static char *plugindir = NULL;

char *plugin_get_dir (void)
{
	if (plugindir == NULL)
		return (PLUGINDIR);
	else
		return (plugindir);
}

void plugin_set_dir (char *dir)
{
	if (plugindir != NULL)
		free (plugindir);

	if (dir == NULL)
		plugindir = NULL;
	else if ((plugindir = strdup (dir)) == NULL)
		syslog (LOG_ERR, "strdup: %s", strerror (errno));
}

/*
 * Returns the number of plugins registered
 */
int plugin_count (void)
{
	int i;
	plugin_t *p;

	for (i = 0, p = first_plugin; p != NULL; p = p->next)
		i++;

	return (i);
}

/*
 * Returns the plugins with the type `type' or NULL if it's not found.
 */
plugin_t *plugin_search (const char *type)
{
	plugin_t *ret;

	if (type == NULL)
		return (NULL);

	for (ret = first_plugin; ret != NULL; ret = ret->next)
		if (strcmp (ret->type, type) == 0)
			break;

	return (ret);
}

/*
 * Returns true if the plugin is loaded (i.e. `exists') and false otherwise.
 * This is used in `configfile.c' to skip sections that are not needed..
 */
int plugin_exists (char *type)
{
	if (plugin_search (type) == NULL)
		return (0);
	else
		return (1);
}

/*
 * (Try to) load the shared object `file'. Won't complain if it isn't a shared
 * object, but it will bitch about a shared object not having a
 * ``module_register'' symbol..
 */
int plugin_load_file (char *file)
{
	lt_dlhandle dlh;
	void (*reg_handle) (void);

	lt_dlinit ();
	lt_dlerror (); /* clear errors */

	if ((dlh = lt_dlopen (file)) == NULL)
		return (1);

	if ((reg_handle = lt_dlsym (dlh, "module_register")) == NULL)
	{
		syslog (LOG_WARNING, "Couldn't find symbol ``module_register'' in ``%s'': %s\n",
				file, lt_dlerror ());
		lt_dlclose (dlh);
		return (-1);
	}

	(*reg_handle) ();

	return (0);
}

#define BUFSIZE 512
int plugin_load (const char *type)
{
	DIR  *dh;
	char *dir;
	char  filename[BUFSIZE];
	char  typename[BUFSIZE];
	int   typename_len;
	int   ret;
	struct stat    statbuf;
	struct dirent *de;

	dir = plugin_get_dir ();
	ret = 1;

	/* don't load twice */
	if (plugin_search (type) != NULL)
		return (0);

	/* `cpu' should not match `cpufreq'. To solve this we add `.so' to the
	 * type when matching the filename */
	if (snprintf (typename, BUFSIZE, "%s.so", type) >= BUFSIZE)
	{
		syslog (LOG_WARNING, "snprintf: truncated: `%s.so'", type);
		return (-1);
	}
	typename_len = strlen (typename);

	if ((dh = opendir (dir)) == NULL)
	{
		syslog (LOG_ERR, "opendir (%s): %s", dir, strerror (errno));
		return (-1);
	}

	while ((de = readdir (dh)) != NULL)
	{
		if (strncasecmp (de->d_name, typename, typename_len))
			continue;

		if (snprintf (filename, BUFSIZE, "%s/%s", dir, de->d_name) >= BUFSIZE)
		{
			syslog (LOG_WARNING, "snprintf: truncated: `%s/%s'", dir, de->d_name);
			continue;
		}

		if (lstat (filename, &statbuf) == -1)
		{
			syslog (LOG_WARNING, "stat %s: %s", filename, strerror (errno));
			continue;
		}
		else if (!S_ISREG (statbuf.st_mode))
		{
			/* don't follow symlinks */
			continue;
		}

		if (plugin_load_file (filename) == 0)
		{
			/* success */
			ret = 0;
			break;
		}
	}

	closedir (dh);

	return (ret);
}

/*
 * (Try to) load all plugins in `dir'. Returns the number of loaded plugins..
 */
int plugin_load_all (char *dir)
{
	DIR *dh;
	struct dirent *de;
	char filename[BUFSIZE];
	struct stat statbuf;

	if (dir == NULL)
		dir = plugin_get_dir ();
	else
		plugin_set_dir (dir);

	if ((dh = opendir (dir)) == NULL)
	{
		syslog (LOG_ERR, "opendir (%s): %s", dir, strerror (errno));
		return (0);
	}

	while ((de = readdir (dh)) != NULL)
	{
		if (snprintf (filename, BUFSIZE, "%s/%s", dir, de->d_name) >= BUFSIZE)
		{
			syslog (LOG_WARNING, "snprintf: truncated: %s/%s", dir, de->d_name);
			continue;
		}

		if (lstat (filename, &statbuf) == -1)
		{
			syslog (LOG_WARNING, "stat %s: %s", filename, strerror (errno));
			continue;
		}
		else if (!S_ISREG (statbuf.st_mode))
		{
			continue;
		}

		plugin_load_file (filename);
	}

	closedir (dh);

	return (plugin_count ());
}
#undef BUFSIZE

/*
 * Call `init' on all plugins (if given)
 */
void plugin_init_all (void)
{
	plugin_t *p;

	for (p = first_plugin; p != NULL; p = p->next)
		if (p->init != NULL)
			(*p->init) ();
}

/*
 * Call `read' on all plugins (if given)
 */
void plugin_read_all (void)
{
	plugin_t *p;

	for (p = first_plugin; p != NULL; p = p->next)
		if (p->read != NULL)
			(*p->read) ();
}

/*
 * Add plugin to the linked list of registered plugins.
 */
void plugin_register (char *type,
		void (*init) (void),
		void (*read) (void),
		void (*write) (char *, char *, char *))
{
	plugin_t *p;

	if (plugin_search (type) != NULL)
		return;

#ifdef HAVE_LIBRRD
	if ((operating_mode == MODE_LOCAL) || (operating_mode == MODE_CLIENT))
#endif
		if (read == NULL)
			syslog (LOG_NOTICE, "Plugin `%s' doesn't provide a read function.", type);

	if ((p = (plugin_t *) malloc (sizeof (plugin_t))) == NULL)
		return;

	if ((p->type = strdup (type)) == NULL)
	{
		free (p);
		return;
	}

	p->init  = init;
	p->read  = read;
	p->write = write;

	p->next = first_plugin;
	first_plugin = p;
}

/*
 * Send received data back to the plugin/module which will append DS
 * definitions and pass it on to ``rrd_update_file''.
 */
#ifdef HAVE_LIBRRD
void plugin_write (char *host, char *type, char *inst, char *val)
{
	plugin_t *p;

	if ((p = plugin_search (type)) == NULL)
		return;

	if (p->write == NULL)
		return;

	(*p->write) (host, inst, val);
}
#endif /* HAVE_LIBRRD */

/*
 * Receive data from the plugin/module and get it somehow to ``plugin_write'':
 * Either using ``multicast_send'' (when in network/client mode) or call it
 * directly (in local mode).
 */
void plugin_submit (char *type, char *inst, char *val)
{
#ifdef HAVE_LIBRRD
	if (operating_mode == MODE_LOCAL)
		plugin_write (NULL, type, inst, val);
	else if (operating_mode == MODE_CLIENT)
		multicast_send (type, inst, val);
	else /* operating_mode == MODE_SERVER */
		syslog (LOG_ERR, "WTF is the server doing in ``plugin_submit''?!?\n");
#else
	multicast_send (type, inst, val);
#endif
}
