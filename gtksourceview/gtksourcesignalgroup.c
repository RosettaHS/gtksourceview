/* gtk_source-signal-group.c
 *
 * Copyright (C) 2015 Christian Hergert <christian@hergert.me>
 * Copyright (C) 2015 Garrett Regier <garrettregier@gmail.com>
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <glib/gi18n.h>

#include "gtksourcesignalgroup-private.h"

/*
 * GtkSourceSignalGroup:
 * 
 * Manage a collection of signals on a #GObject.
 *
 * #GtkSourceSignalGroup manages to simplify the process of connecting
 * many signals to a #GObject as a group. As such there is no API
 * to disconnect a signal from the group.
 *
 * In particular, this allows you to:
 *
 *  - Change the target instance, which automatically causes disconnection
 *    of the signals from the old instance and connecting to the new instance.
 *  - Block and unblock signals as a group
 *  - Ensuring that blocked state transfers across target instances.
 *
 * One place you might want to use such a structure is with #GtkTextView and
 * #GtkTextBuffer. Often times, you'll need to connect to many signals on
 * #GtkTextBuffer from a #GtkTextView subclass. This allows you to create a
 * signal group during instance construction, simply bind the
 * #GtkTextView:buffer property to #GtkSourceSignalGroup:target and connect
 * all the signals you need. When the #GtkTextView:buffer property changes
 * all of the signals will be transitioned correctly.
 */

struct _GtkSourceSignalGroup
{
  GObject     parent_instance;

  GWeakRef    target_ref;
  GPtrArray  *handlers;
  GType       target_type;
  gsize       block_count;

  guint       has_bound_at_least_once : 1;
};

struct _GtkSourceSignalGroupClass
{
  GObjectClass parent_class;

  void (*bind) (GtkSourceSignalGroup *self,
                GObject           *target);
};

typedef struct
{
  GtkSourceSignalGroup *group;
  gulong                handler_id;
  GClosure             *closure;
  guint                 signal_id;
  GQuark                signal_detail;
  guint                 connect_after : 1;
} SignalHandler;

G_DEFINE_TYPE (GtkSourceSignalGroup, gtk_source_signal_group, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_TARGET,
  PROP_TARGET_TYPE,
  LAST_PROP
};

enum {
  BIND,
  UNBIND,
  LAST_SIGNAL
};

static GParamSpec *properties [LAST_PROP];
static guint signals [LAST_SIGNAL];

static void
gtk_source_signal_group_set_target_type (GtkSourceSignalGroup *self,
                                         GType                 target_type)
{
	g_assert (GTK_SOURCE_IS_SIGNAL_GROUP (self));
	g_assert (g_type_is_a (target_type, G_TYPE_OBJECT));

	self->target_type = target_type;

	/* The class must be created at least once for the signals
	 * to be registered, otherwise g_signal_parse_name() will fail
	 */
	if (G_TYPE_IS_INTERFACE (target_type))
	{
		if (g_type_default_interface_peek (target_type) == NULL)
			g_type_default_interface_unref (g_type_default_interface_ref (target_type));
	}
	else
	{
		if (g_type_class_peek (target_type) == NULL)
			g_type_class_unref (g_type_class_ref (target_type));
	}
}

static void
gtk_source_signal_group_gc_handlers (GtkSourceSignalGroup *self)
{
	g_assert (GTK_SOURCE_IS_SIGNAL_GROUP (self));

	/* Remove any handlers for which the closures have become invalid. We do
	 * this cleanup lazily to avoid situations where we could have disposal
	 * active on both the signal group and the peer object.
	 */

	for (guint i = self->handlers->len; i > 0; i--)
	{
		const SignalHandler *handler = g_ptr_array_index (self->handlers, i - 1);

		g_assert (handler != NULL);
		g_assert (handler->closure != NULL);

		if (handler->closure->is_invalid)
		{
			g_ptr_array_remove_index (self->handlers, i - 1);
		}
	}
}

static void
gtk_source_signal_group__target_weak_notify (gpointer  data,
                                             GObject  *where_object_was)
{
	GtkSourceSignalGroup *self = data;

	g_assert (GTK_SOURCE_IS_SIGNAL_GROUP (self));
	g_assert (where_object_was != NULL);

	g_weak_ref_set (&self->target_ref, NULL);

	for (guint i = 0; i < self->handlers->len; i++)
	{
		SignalHandler *handler = g_ptr_array_index (self->handlers, i);

		handler->handler_id = 0;
	}

	g_signal_emit (self, signals [UNBIND], 0);
	g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_TARGET]);
}

static void
gtk_source_signal_group_bind_handler (GtkSourceSignalGroup *self,
                                      SignalHandler        *handler,
                                      GObject              *target)
{
	g_assert (self != NULL);
	g_assert (G_IS_OBJECT (target));
	g_assert (handler != NULL);
	g_assert (handler->signal_id != 0);
	g_assert (handler->closure != NULL);
	g_assert (handler->closure->is_invalid == 0);
	g_assert (handler->handler_id == 0);

	handler->handler_id = g_signal_connect_closure_by_id (target,
	                                                      handler->signal_id,
	                                                      handler->signal_detail,
	                                                      handler->closure,
	                                                      handler->connect_after);

	g_assert (handler->handler_id != 0);

	for (guint i = 0; i < self->block_count; i++)
		g_signal_handler_block (target, handler->handler_id);
}

static void
gtk_source_signal_group_bind (GtkSourceSignalGroup *self,
                              GObject              *target)
{
	GObject *hold = NULL;

	g_assert (GTK_SOURCE_IS_SIGNAL_GROUP (self));
	g_assert (!target || G_IS_OBJECT (target));

	if (target == NULL)
		return;

	self->has_bound_at_least_once = TRUE;

	hold = g_object_ref (target);

	g_weak_ref_set (&self->target_ref, hold);
	g_object_weak_ref (hold, gtk_source_signal_group__target_weak_notify, self);

	gtk_source_signal_group_gc_handlers (self);

	for (guint i = 0; i < self->handlers->len; i++)
	{
		SignalHandler *handler = g_ptr_array_index (self->handlers, i);

		gtk_source_signal_group_bind_handler (self, handler, hold);
	}

	g_signal_emit (self, signals [BIND], 0, hold);

	g_clear_object (&hold);
}

static void
gtk_source_signal_group_unbind (GtkSourceSignalGroup *self)
{
	GObject *target = NULL;

	g_return_if_fail (GTK_SOURCE_IS_SIGNAL_GROUP (self));

	target = g_weak_ref_get (&self->target_ref);

	/* Target may be NULL by this point, as we got notified of its destruction.
	 * However, if we're early enough, we may get a full reference back and can
	 * cleanly disconnect our connections.
	 */

	if (target != NULL)
	{
		g_weak_ref_set (&self->target_ref, NULL);

		/* Let go of our weak reference now that we have a full reference
		 * for the life of this function.
		 */
		g_object_weak_unref (target,
				     gtk_source_signal_group__target_weak_notify,
				     self);
	}

	gtk_source_signal_group_gc_handlers (self);

	for (guint i = 0; i < self->handlers->len; i++)
	{
		SignalHandler *handler;
		gulong handler_id;

		handler = g_ptr_array_index (self->handlers, i);

		g_assert (handler != NULL);
		g_assert (handler->signal_id != 0);
		g_assert (handler->closure != NULL);

		handler_id = handler->handler_id;
		handler->handler_id = 0;

		/*
		 * If @target is NULL, we lost a race to cleanup the weak
		 * instance and the signal connections have already been
		 * finalized and therefore nothing to do.
		 */

		if (target != NULL && handler_id != 0)
			g_signal_handler_disconnect (target, handler_id);
	}

	g_signal_emit (self, signals [UNBIND], 0);

	g_clear_object (&target);
}

static gboolean
gtk_source_signal_group_check_target_type (GtkSourceSignalGroup *self,
                                           gpointer              target)
{
	if ((target != NULL) &&
	    !g_type_is_a (G_OBJECT_TYPE (target), self->target_type))
	{
		g_critical ("Failed to set GtkSourceSignalGroup of target type %s "
                            "using target %p of type %s",
		            g_type_name (self->target_type),
		            target, G_OBJECT_TYPE_NAME (target));
		return FALSE;
	}

	return TRUE;
}

/**
 * gtk_source_signal_group_block:
 * @self: the #GtkSourceSignalGroup
 *
 * Blocks all signal handlers managed by @self so they will not
 * be called during any signal emissions. Must be unblocked exactly
 * the same number of times it has been blocked to become active again.
 *
 * This blocked state will be kept across changes of the target instance.
 *
 * See: [func@GObject.signal_handler_block].
 */
void
gtk_source_signal_group_block (GtkSourceSignalGroup *self)
{
	GObject *target = NULL;

	g_return_if_fail (GTK_SOURCE_IS_SIGNAL_GROUP (self));
	g_return_if_fail (self->block_count != G_MAXSIZE);

	self->block_count++;

	target = g_weak_ref_get (&self->target_ref);

	if (target == NULL)
		return;

	for (guint i = 0; i < self->handlers->len; i++)
	{
		const SignalHandler *handler = g_ptr_array_index (self->handlers, i);

		g_assert (handler != NULL);
		g_assert (handler->signal_id != 0);
		g_assert (handler->closure != NULL);
		g_assert (handler->handler_id != 0);

		g_signal_handler_block (target, handler->handler_id);
	}

	g_clear_object (&target);
}

/**
 * gtk_source_signal_group_unblock:
 * @self: the #GtkSourceSignalGroup
 *
 * Unblocks all signal handlers managed by @self so they will be
 * called again during any signal emissions unless it is blocked
 * again. Must be unblocked exactly the same number of times it
 * has been blocked to become active again.
 *
 * See: [func@GObject.signal_handler_unblock].
 */
void
gtk_source_signal_group_unblock (GtkSourceSignalGroup *self)
{
	GObject *target = NULL;

	g_return_if_fail (GTK_SOURCE_IS_SIGNAL_GROUP (self));
	g_return_if_fail (self->block_count != 0);

	self->block_count--;

	target = g_weak_ref_get (&self->target_ref);

	if (target == NULL)
		return;

	for (guint i = 0; i < self->handlers->len; i++)
	{
		const SignalHandler *handler = g_ptr_array_index (self->handlers, i);

		g_assert (handler != NULL);
		g_assert (handler->signal_id != 0);
		g_assert (handler->closure != NULL);
		g_assert (handler->handler_id != 0);

		g_signal_handler_unblock (target, handler->handler_id);
	}

	g_clear_object (&target);
}

/**
 * gtk_source_signal_group_get_target:
 * @self: the #GtkSourceSignalGroup
 *
 * Gets the target instance used when connecting signals.
 *
 * Returns: (nullable) (transfer none) (type GObject): The target instance.
 */
gpointer
gtk_source_signal_group_get_target (GtkSourceSignalGroup *self)
{
	GObject *target = NULL;

	g_return_val_if_fail (GTK_SOURCE_IS_SIGNAL_GROUP (self), NULL);

	target = g_weak_ref_get (&self->target_ref);

	if (target == NULL || target->ref_count < 2)
	{
		/*
		 * It is expected that this is called from a thread that owns a reference to
		 * the target, so we can pass back a borrowed reference. However, to ensure
		 * that we aren't racing in finalization of @target, we must ensure that the
		 * ref_count >= 2 (as our get just incremented by one).
		 */
		g_clear_object (&target);
	}
	else
	{
		/* Unref and pass back a borrowed reference. This looks unsafe, but is safe
		 * because of our reference check above, so much as the assertion holds that
		 * the caller obeyed the ownership rules of this class.
		 */
		g_object_unref (target);
	}

	return target;
}

/**
 * gtk_source_signal_group_set_target:
 * @self: the #GtkSourceSignalGroup.
 * @target: (nullable) (type GObject): The target instance used
 *     when connecting signals.
 *
 * Sets the target instance used when connecting signals. Any signal
 * that has been registered with gtk_source_signal_group_connect_object() or
 * similar functions will be connected to this object.
 *
 * If the target instance was previously set, signals will be
 * disconnected from that object prior to connecting to @target.
 */
void
gtk_source_signal_group_set_target (GtkSourceSignalGroup *self,
                                    gpointer              target)
{
	GObject *object = NULL;

	g_return_if_fail (GTK_SOURCE_IS_SIGNAL_GROUP (self));

	object = g_weak_ref_get (&self->target_ref);

	if (object == (GObject *)target)
		goto release;

	if (!gtk_source_signal_group_check_target_type (self, target))
		goto release;

	/* Only emit unbind if we've ever called bind */
	if (self->has_bound_at_least_once)
		gtk_source_signal_group_unbind (self);

	gtk_source_signal_group_bind (self, target);

	g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_TARGET]);

release:
	g_clear_object (&object);
}

static void
signal_handler_free (gpointer data)
{
	SignalHandler *handler = data;

	if (handler->closure != NULL)
		g_closure_invalidate (handler->closure);

	handler->handler_id = 0;
	handler->signal_id = 0;
	handler->signal_detail = 0;
	g_clear_pointer (&handler->closure, g_closure_unref);
	g_slice_free (SignalHandler, handler);
}

static void
gtk_source_signal_group_constructed (GObject *object)
{
	GtkSourceSignalGroup *self = (GtkSourceSignalGroup *)object;
	GObject *target = g_weak_ref_get (&self->target_ref);

	if (!gtk_source_signal_group_check_target_type (self, target))
		gtk_source_signal_group_set_target (self, NULL);

	G_OBJECT_CLASS (gtk_source_signal_group_parent_class)->constructed (object);

	g_clear_object (&target);
}

static void
gtk_source_signal_group_dispose (GObject *object)
{
	GtkSourceSignalGroup *self = (GtkSourceSignalGroup *)object;

	gtk_source_signal_group_gc_handlers (self);

	if (self->has_bound_at_least_once)
		gtk_source_signal_group_unbind (self);

	g_clear_pointer (&self->handlers, g_ptr_array_unref);

	G_OBJECT_CLASS (gtk_source_signal_group_parent_class)->dispose (object);
}

static void
gtk_source_signal_group_finalize (GObject *object)
{
	GtkSourceSignalGroup *self = (GtkSourceSignalGroup *)object;

	g_weak_ref_clear (&self->target_ref);

	G_OBJECT_CLASS (gtk_source_signal_group_parent_class)->finalize (object);
}

static void
gtk_source_signal_group_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
	GtkSourceSignalGroup *self = GTK_SOURCE_SIGNAL_GROUP (object);

	switch (prop_id)
	{
	case PROP_TARGET:
		g_value_take_object (value, g_weak_ref_get (&self->target_ref));
		break;

	case PROP_TARGET_TYPE:
		g_value_set_gtype (value, self->target_type);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static void
gtk_source_signal_group_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
	GtkSourceSignalGroup *self = GTK_SOURCE_SIGNAL_GROUP (object);

	switch (prop_id)
	{
	case PROP_TARGET:
		gtk_source_signal_group_set_target (self, g_value_get_object (value));
		break;

	case PROP_TARGET_TYPE:
		gtk_source_signal_group_set_target_type (self, g_value_get_gtype (value));
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static void
gtk_source_signal_group_class_init (GtkSourceSignalGroupClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->constructed = gtk_source_signal_group_constructed;
	object_class->dispose = gtk_source_signal_group_dispose;
	object_class->finalize = gtk_source_signal_group_finalize;
	object_class->get_property = gtk_source_signal_group_get_property;
	object_class->set_property = gtk_source_signal_group_set_property;

	/**
	 * GtkSourceSignalGroup:target
	 *
	 * The target instance used when connecting signals.
	 */
	properties [PROP_TARGET] =
		g_param_spec_object ("target",
		                     "Target",
		                     "The target instance used when connecting signals.",
		                     G_TYPE_OBJECT,
		                     (G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));

	/**
	 * GtkSourceSignalGroup:target-type
	 *
	 * The GType of the target property.
	 */
	properties [PROP_TARGET_TYPE] =
		g_param_spec_gtype ("target-type",
		                    "Target Type",
		                    "The GType of the target property.",
		                    G_TYPE_OBJECT,
		                    (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

	g_object_class_install_properties (object_class, LAST_PROP, properties);

	/**
	 * GtkSourceSignalGroup::bind:
	 * @self: the #GtkSourceSignalGroup
	 * @instance: a #GObject
	 *
	 * This signal is emitted when the target instance of @self
	 * is set to a new #GObject.
	 *
	 * This signal will only be emitted if the target of @self is non-%NULL.
	 */
	signals [BIND] =
		g_signal_new ("bind",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL, NULL,
		              G_TYPE_NONE,
		              1,
		              G_TYPE_OBJECT);

	/**
	 * GtkSourceSignalGroup::unbind:
	 * @self: a #GtkSourceSignalGroup
	 *
	 * This signal is emitted when the target instance of @self
	 * is set to a new #GObject.
	 *
	 * This signal will only be emitted if the previous target
	 * of @self is non-%NULL.
	 */
	signals [UNBIND] =
		g_signal_new ("unbind",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL, NULL,
		              G_TYPE_NONE,
		              0);
}

static void
gtk_source_signal_group_init (GtkSourceSignalGroup *self)
{
	self->handlers = g_ptr_array_new_with_free_func (signal_handler_free);
	self->target_type = G_TYPE_OBJECT;
}

/**
 * gtk_source_signal_group_new:
 * @target_type: the #GType of the target instance.
 *
 * Creates a new #GtkSourceSignalGroup for target instances of @target_type.
 *
 * Returns: a new #GtkSourceSignalGroup
 */
GtkSourceSignalGroup *
gtk_source_signal_group_new (GType target_type)
{
	g_return_val_if_fail (g_type_is_a (target_type, G_TYPE_OBJECT), NULL);

	return g_object_new (GTK_SOURCE_TYPE_SIGNAL_GROUP,
	                     "target-type", target_type,
	                     NULL);
}

static void
gtk_source_signal_group_connect_full (GtkSourceSignalGroup *self,
                                      const gchar          *detailed_signal,
                                      GCallback             callback,
                                      gpointer              data,
                                      GClosureNotify        notify,
                                      GConnectFlags         flags,
                                      gboolean              is_object)
{
	GObject *target = NULL;
	SignalHandler *handler;
	GClosure *closure;
	guint signal_id;
	GQuark signal_detail;

	g_return_if_fail (GTK_SOURCE_IS_SIGNAL_GROUP (self));
	g_return_if_fail (detailed_signal != NULL);
	g_return_if_fail (g_signal_parse_name (detailed_signal, self->target_type,
					       &signal_id, &signal_detail, TRUE) != 0);
	g_return_if_fail (callback != NULL);
	g_return_if_fail (!is_object || G_IS_OBJECT (data));

	if ((flags & G_CONNECT_SWAPPED) != 0)
		closure = g_cclosure_new_swap (callback, data, notify);
	else
		closure = g_cclosure_new (callback, data, notify);

	handler = g_slice_new0 (SignalHandler);
	handler->group = self;
	handler->signal_id = signal_id;
	handler->signal_detail = signal_detail;
	handler->closure = g_closure_ref (closure);
	handler->connect_after = ((flags & G_CONNECT_AFTER) != 0);

	g_closure_sink (closure);

	if (is_object)
	{
		/* Set closure->is_invalid when data is disposed. We only track this to avoid
		 * reconnecting in the future. However, we do a round of cleanup when ever we
		 * connect a new object or the target changes to GC the old handlers.
		 */
		g_object_watch_closure (data, closure);
	}

	g_ptr_array_add (self->handlers, handler);

	target = g_weak_ref_get (&self->target_ref);
	if (target != NULL)
		gtk_source_signal_group_bind_handler (self, handler, target);

	/* Lazily remove any old handlers on connect */
	gtk_source_signal_group_gc_handlers (self);

	g_clear_object (&target);
}

/**
 * gtk_source_signal_group_connect_object: (skip)
 * @self: a #GtkSourceSignalGroup
 * @detailed_signal: a string of the form "signal-name::detail"
 * @c_handler: (scope notified): the #GCallback to connect
 * @object: the #GObject to pass as data to @callback calls
 *
 * Connects @callback to the signal @detailed_signal
 * on the target object of @self.
 *
 * Ensures that the @object stays alive during the call to @callback
 * by temporarily adding a reference count. When the @object is destroyed
 * the signal handler will automatically be removed.
 *
 * See: [func@GObject.signal_connect_object].
 */
void
gtk_source_signal_group_connect_object (GtkSourceSignalGroup *self,
                                        const gchar          *detailed_signal,
                                        GCallback             c_handler,
                                        gpointer              object,
                                        GConnectFlags         flags)
{
	g_return_if_fail (G_IS_OBJECT (object));

	gtk_source_signal_group_connect_full (self, detailed_signal, c_handler,
	                                      object, NULL, flags, TRUE);
}

/**
 * gtk_source_signal_group_connect_data:
 * @self: a #GtkSourceSignalGroup
 * @detailed_signal: a string of the form "signal-name::detail"
 * @c_handler: (scope notified) (closure data) (destroy notify): the #GCallback to connect
 * @data: the data to pass to @callback calls
 * @notify: function to be called when disposing of @self
 * @flags: the flags used to create the signal connection
 *
 * Connects @callback to the signal @detailed_signal
 * on the target instance of @self.
 *
 * See: [func@GObject.signal_connect_data].
 */
void
gtk_source_signal_group_connect_data (GtkSourceSignalGroup *self,
                                      const gchar          *detailed_signal,
                                      GCallback             c_handler,
                                      gpointer              data,
                                      GClosureNotify        notify,
                                      GConnectFlags         flags)
{
	gtk_source_signal_group_connect_full (self, detailed_signal, c_handler,
	                                      data, notify, flags, FALSE);
}

/**
 * gtk_source_signal_group_connect: (skip)
 * @self: a #GtkSourceSignalGroup
 * @detailed_signal: a string of the form "signal-name::detail"
 * @c_handler: (scope notified): the #GCallback to connect
 * @data: the data to pass to @callback calls
 *
 * Connects @callback to the signal @detailed_signal
 * on the target instance of @self.
 *
 * See: [func@GObject.signal_connect].
 */
void
gtk_source_signal_group_connect (GtkSourceSignalGroup *self,
                                 const gchar          *detailed_signal,
                                 GCallback             c_handler,
                                 gpointer              data)
{
	gtk_source_signal_group_connect_full (self, detailed_signal, c_handler, data, NULL,
	                                      0, FALSE);
}

/**
 * gtk_source_signal_group_connect_after: (skip)
 * @self: a #GtkSourceSignalGroup
 * @detailed_signal: a string of the form "signal-name::detail"
 * @c_handler: (scope notified): the #GCallback to connect
 * @data: the data to pass to @callback calls
 *
 * Connects @callback to the signal @detailed_signal
 * on the target instance of @self.
 *
 * The @callback will be called after the default handler of the signal.
 *
 * See: [func@GObject.signal_connect_after].
 */
void
gtk_source_signal_group_connect_after (GtkSourceSignalGroup *self,
                                       const gchar          *detailed_signal,
                                       GCallback             c_handler,
                                       gpointer              data)
{
	gtk_source_signal_group_connect_full (self, detailed_signal, c_handler,
	                                      data, NULL, G_CONNECT_AFTER, FALSE);
}

/**
 * gtk_source_signal_group_connect_swapped:
 * @self: a #GtkSourceSignalGroup
 * @detailed_signal: a string of the form "signal-name::detail"
 * @c_handler: (scope async): the #GCallback to connect
 * @data: the data to pass to @callback calls
 *
 * Connects @callback to the signal @detailed_signal
 * on the target instance of @self.
 *
 * The instance on which the signal is emitted and @data
 * will be swapped when calling @callback.
 *
 * See: [func@GObject.signal_connect_swapped].
 */
void
gtk_source_signal_group_connect_swapped (GtkSourceSignalGroup *self,
                                         const gchar          *detailed_signal,
                                         GCallback             c_handler,
                                         gpointer              data)
{
	gtk_source_signal_group_connect_full (self, detailed_signal, c_handler, data, NULL,
	                                      G_CONNECT_SWAPPED, FALSE);
}
