/*
 * Server-side pipe management
 *
 * Copyright (C) 1998 Alexandre Julliard
 * Copyright (C) 2001 Mike McCormack
 * Copyright 2016 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <time.h>
#include <unistd.h>
#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winioctl.h"

#include "file.h"
#include "handle.h"
#include "thread.h"
#include "request.h"
#include "security.h"

enum pipe_state
{
    ps_idle_server,
    ps_wait_open,
    ps_connected_server,
    ps_wait_disconnect,
    ps_wait_connect
};

struct named_pipe;

struct pipe_message
{
    struct list          entry;      /* entry in message queue */
    data_size_t          read_pos;   /* already read bytes */
    struct iosb         *iosb;       /* message iosb */
    struct async        *async;      /* async of pending write */
};

struct pipe_end
{
    struct object        obj;        /* object header */
    struct fd           *fd;         /* pipe file descriptor */
    unsigned int         flags;      /* pipe flags */
    struct pipe_end     *connection; /* the other end of the pipe */
    data_size_t          buffer_size;/* size of buffered data that doesn't block caller */
    struct list          message_queue;
    struct async_queue  *read_q;     /* read queue */
    struct async_queue  *write_q;    /* write queue */
};

struct pipe_server
{
    struct pipe_end      pipe_end;   /* common header for pipe_client and pipe_server */
    struct fd           *ioctl_fd;   /* file descriptor for ioctls when not connected */
    struct list          entry;      /* entry in named pipe servers list */
    enum pipe_state      state;      /* server state */
    struct pipe_client  *client;     /* client that this server is connected to */
    struct named_pipe   *pipe;
    struct timeout_user *flush_poll;
    unsigned int         options;    /* pipe options */
};

struct pipe_client
{
    struct pipe_end      pipe_end;   /* common header for pipe_client and pipe_server */
    struct pipe_server  *server;     /* server that this client is connected to */
    unsigned int         flags;      /* file flags */
};

struct named_pipe
{
    struct object       obj;         /* object header */
    unsigned int        flags;
    unsigned int        sharing;
    unsigned int        maxinstances;
    unsigned int        outsize;
    unsigned int        insize;
    unsigned int        instances;
    timeout_t           timeout;
    struct list         servers;     /* list of servers using this pipe */
    struct async_queue *waiters;     /* list of clients waiting to connect */
};

struct named_pipe_device
{
    struct object       obj;         /* object header */
    struct fd          *fd;          /* pseudo-fd for ioctls */
    struct namespace   *pipes;       /* named pipe namespace */
};

static void named_pipe_dump( struct object *obj, int verbose );
static unsigned int named_pipe_map_access( struct object *obj, unsigned int access );
static int named_pipe_link_name( struct object *obj, struct object_name *name, struct object *parent );
static struct object *named_pipe_open_file( struct object *obj, unsigned int access,
                                            unsigned int sharing, unsigned int options );
static void named_pipe_destroy( struct object *obj );

static const struct object_ops named_pipe_ops =
{
    sizeof(struct named_pipe),    /* size */
    named_pipe_dump,              /* dump */
    no_get_type,                  /* get_type */
    no_add_queue,                 /* add_queue */
    NULL,                         /* remove_queue */
    NULL,                         /* signaled */
    NULL,                         /* satisfied */
    no_signal,                    /* signal */
    no_get_fd,                    /* get_fd */
    named_pipe_map_access,        /* map_access */
    default_get_sd,               /* get_sd */
    default_set_sd,               /* set_sd */
    no_lookup_name,               /* lookup_name */
    named_pipe_link_name,         /* link_name */
    default_unlink_name,          /* unlink_name */
    named_pipe_open_file,         /* open_file */
    no_close_handle,              /* close_handle */
    named_pipe_destroy            /* destroy */
};

/* common server and client pipe end functions */
static enum server_fd_type pipe_end_get_fd_type( struct fd *fd );
static obj_handle_t pipe_end_read( struct fd *fd, struct async *async, file_pos_t pos );
static obj_handle_t pipe_end_write( struct fd *fd, struct async *async_data, file_pos_t pos );
static void pipe_end_queue_async( struct fd *fd, struct async *async, int type, int count );
static void pipe_end_reselect_async( struct fd *fd, struct async_queue *queue );

/* server end functions */
static void pipe_server_dump( struct object *obj, int verbose );
static struct fd *pipe_server_get_fd( struct object *obj );
static void pipe_server_destroy( struct object *obj);
static obj_handle_t pipe_server_flush( struct fd *fd, struct async *async );
static obj_handle_t pipe_server_ioctl( struct fd *fd, ioctl_code_t code, struct async *async );

static const struct object_ops pipe_server_ops =
{
    sizeof(struct pipe_server),   /* size */
    pipe_server_dump,             /* dump */
    no_get_type,                  /* get_type */
    add_queue,                    /* add_queue */
    remove_queue,                 /* remove_queue */
    default_fd_signaled,          /* signaled */
    no_satisfied,                 /* satisfied */
    no_signal,                    /* signal */
    pipe_server_get_fd,           /* get_fd */
    default_fd_map_access,        /* map_access */
    default_get_sd,               /* get_sd */
    default_set_sd,               /* set_sd */
    no_lookup_name,               /* lookup_name */
    no_link_name,                 /* link_name */
    NULL,                         /* unlink_name */
    no_open_file,                 /* open_file */
    fd_close_handle,              /* close_handle */
    pipe_server_destroy           /* destroy */
};

static const struct fd_ops pipe_server_fd_ops =
{
    default_fd_get_poll_events,   /* get_poll_events */
    default_poll_event,           /* poll_event */
    pipe_end_get_fd_type,         /* get_fd_type */
    pipe_end_read,                /* read */
    pipe_end_write,               /* write */
    pipe_server_flush,            /* flush */
    pipe_server_ioctl,            /* ioctl */
    pipe_end_queue_async,         /* queue_async */
    pipe_end_reselect_async       /* reselect_async */
};

/* client end functions */
static void pipe_client_dump( struct object *obj, int verbose );
static int pipe_client_signaled( struct object *obj, struct wait_queue_entry *entry );
static struct fd *pipe_client_get_fd( struct object *obj );
static void pipe_client_destroy( struct object *obj );
static obj_handle_t pipe_client_flush( struct fd *fd, struct async *async );
static obj_handle_t pipe_client_ioctl( struct fd *fd, ioctl_code_t code, struct async *async );

static const struct object_ops pipe_client_ops =
{
    sizeof(struct pipe_client),   /* size */
    pipe_client_dump,             /* dump */
    no_get_type,                  /* get_type */
    add_queue,                    /* add_queue */
    remove_queue,                 /* remove_queue */
    pipe_client_signaled,         /* signaled */
    no_satisfied,                 /* satisfied */
    no_signal,                    /* signal */
    pipe_client_get_fd,           /* get_fd */
    default_fd_map_access,        /* map_access */
    default_get_sd,               /* get_sd */
    default_set_sd,               /* set_sd */
    no_lookup_name,               /* lookup_name */
    no_link_name,                 /* link_name */
    NULL,                         /* unlink_name */
    no_open_file,                 /* open_file */
    fd_close_handle,              /* close_handle */
    pipe_client_destroy           /* destroy */
};

static const struct fd_ops pipe_client_fd_ops =
{
    default_fd_get_poll_events,   /* get_poll_events */
    default_poll_event,           /* poll_event */
    pipe_end_get_fd_type,         /* get_fd_type */
    pipe_end_read,                /* read */
    pipe_end_write,               /* write */
    pipe_client_flush,            /* flush */
    pipe_client_ioctl,            /* ioctl */
    pipe_end_queue_async,         /* queue_async */
    pipe_end_reselect_async       /* reselect_async */
};

static void named_pipe_device_dump( struct object *obj, int verbose );
static struct object_type *named_pipe_device_get_type( struct object *obj );
static struct fd *named_pipe_device_get_fd( struct object *obj );
static struct object *named_pipe_device_lookup_name( struct object *obj,
    struct unicode_str *name, unsigned int attr );
static struct object *named_pipe_device_open_file( struct object *obj, unsigned int access,
                                                   unsigned int sharing, unsigned int options );
static void named_pipe_device_destroy( struct object *obj );
static enum server_fd_type named_pipe_device_get_fd_type( struct fd *fd );
static obj_handle_t named_pipe_device_ioctl( struct fd *fd, ioctl_code_t code,
                                             struct async *async );

static const struct object_ops named_pipe_device_ops =
{
    sizeof(struct named_pipe_device), /* size */
    named_pipe_device_dump,           /* dump */
    named_pipe_device_get_type,       /* get_type */
    no_add_queue,                     /* add_queue */
    NULL,                             /* remove_queue */
    NULL,                             /* signaled */
    no_satisfied,                     /* satisfied */
    no_signal,                        /* signal */
    named_pipe_device_get_fd,         /* get_fd */
    no_map_access,                    /* map_access */
    default_get_sd,                   /* get_sd */
    default_set_sd,                   /* set_sd */
    named_pipe_device_lookup_name,    /* lookup_name */
    directory_link_name,              /* link_name */
    default_unlink_name,              /* unlink_name */
    named_pipe_device_open_file,      /* open_file */
    fd_close_handle,                  /* close_handle */
    named_pipe_device_destroy         /* destroy */
};

static const struct fd_ops named_pipe_device_fd_ops =
{
    default_fd_get_poll_events,       /* get_poll_events */
    default_poll_event,               /* poll_event */
    named_pipe_device_get_fd_type,    /* get_fd_type */
    no_fd_read,                       /* read */
    no_fd_write,                      /* write */
    no_fd_flush,                      /* flush */
    named_pipe_device_ioctl,          /* ioctl */
    default_fd_queue_async,           /* queue_async */
    default_fd_reselect_async         /* reselect_async */
};

/* Returns if we handle I/O via server calls. Currently message-mode pipes are handled this way. */
static int use_server_io( struct pipe_end *pipe_end )
{
    return pipe_end->flags & NAMED_PIPE_MESSAGE_STREAM_WRITE;
}

static void named_pipe_dump( struct object *obj, int verbose )
{
    fputs( "Named pipe\n", stderr );
}

static unsigned int named_pipe_map_access( struct object *obj, unsigned int access )
{
    if (access & GENERIC_READ)    access |= STANDARD_RIGHTS_READ;
    if (access & GENERIC_WRITE)   access |= STANDARD_RIGHTS_WRITE | FILE_CREATE_PIPE_INSTANCE;
    if (access & GENERIC_EXECUTE) access |= STANDARD_RIGHTS_EXECUTE;
    if (access & GENERIC_ALL)     access |= STANDARD_RIGHTS_ALL;
    return access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
}

static void pipe_server_dump( struct object *obj, int verbose )
{
    struct pipe_server *server = (struct pipe_server *) obj;
    assert( obj->ops == &pipe_server_ops );
    fprintf( stderr, "Named pipe server pipe=%p state=%d\n", server->pipe, server->state );
}

static void pipe_client_dump( struct object *obj, int verbose )
{
    struct pipe_client *client = (struct pipe_client *) obj;
    assert( obj->ops == &pipe_client_ops );
    fprintf( stderr, "Named pipe client server=%p\n", client->server );
}

static int pipe_client_signaled( struct object *obj, struct wait_queue_entry *entry )
{
    struct pipe_client *client = (struct pipe_client *) obj;

    return client->pipe_end.fd && is_fd_signaled(client->pipe_end.fd);
}

static void named_pipe_destroy( struct object *obj)
{
    struct named_pipe *pipe = (struct named_pipe *) obj;

    assert( list_empty( &pipe->servers ) );
    assert( !pipe->instances );
    free_async_queue( pipe->waiters );
}

static struct fd *pipe_client_get_fd( struct object *obj )
{
    struct pipe_client *client = (struct pipe_client *) obj;
    if (client->pipe_end.fd)
        return (struct fd *) grab_object( client->pipe_end.fd );
    set_error( STATUS_PIPE_DISCONNECTED );
    return NULL;
}

static void set_server_state( struct pipe_server *server, enum pipe_state state )
{
    server->state = state;

    switch(state)
    {
    case ps_connected_server:
    case ps_wait_disconnect:
        assert( server->pipe_end.fd );
        break;
    case ps_wait_open:
    case ps_idle_server:
        assert( !server->pipe_end.fd );
        set_no_fd_status( server->ioctl_fd, STATUS_PIPE_LISTENING );
        break;
    case ps_wait_connect:
        assert( !server->pipe_end.fd );
        set_no_fd_status( server->ioctl_fd, STATUS_PIPE_DISCONNECTED );
        break;
    }
}

static struct fd *pipe_server_get_fd( struct object *obj )
{
    struct pipe_server *server = (struct pipe_server *) obj;

    return (struct fd *)grab_object( server->pipe_end.fd ? server->pipe_end.fd : server->ioctl_fd );
}


static void notify_empty( struct pipe_server *server )
{
    if (!server->flush_poll)
        return;
    assert( server->state == ps_connected_server );
    remove_timeout_user( server->flush_poll );
    server->flush_poll = NULL;
    fd_async_wake_up( server->pipe_end.fd, ASYNC_TYPE_WAIT, STATUS_SUCCESS );
}

static void wake_message( struct pipe_message *message )
{
    struct async *async = message->async;

    message->async = NULL;
    message->iosb->status = STATUS_SUCCESS;
    message->iosb->result = message->iosb->in_size;
    if (async)
    {
        async_terminate( async, message->iosb->result ? STATUS_ALERTED : STATUS_SUCCESS );
        release_object( async );
    }
}

static void free_message( struct pipe_message *message )
{
    list_remove( &message->entry );
    if (message->iosb) release_object( message->iosb );
    free( message );
}

static void pipe_end_disconnect( struct pipe_end *pipe_end, unsigned int status )
{
    struct pipe_end *connection = pipe_end->connection;

    pipe_end->connection = NULL;

    if (use_server_io( pipe_end ))
    {
        struct pipe_message *message, *next;
        struct async *async;
        if (pipe_end->fd) fd_async_wake_up( pipe_end->fd, ASYNC_TYPE_WAIT, status );
        async_wake_up( pipe_end->read_q, status );
        LIST_FOR_EACH_ENTRY_SAFE( message, next, &pipe_end->message_queue, struct pipe_message, entry )
        {
            async = message->async;
            if (async || status == STATUS_PIPE_DISCONNECTED) free_message( message );
            if (!async) continue;
            async_terminate( async, status );
            release_object( async );
        }
        if (status == STATUS_PIPE_DISCONNECTED) set_fd_signaled( pipe_end->fd, 0 );
    }
    if (connection)
    {
        connection->connection = NULL;
        pipe_end_disconnect( connection, status );
    }
}

static void do_disconnect( struct pipe_server *server )
{
    /* we may only have a server fd, if the client disconnected */
    if (server->client)
    {
        assert( server->client->server == server );
        assert( server->client->pipe_end.fd );
        if (!use_server_io( &server->pipe_end ))
        {
            release_object( server->client->pipe_end.fd );
            server->client->pipe_end.fd = NULL;
        }
    }
    assert( server->pipe_end.fd );
    if (!use_server_io( &server->pipe_end ))
        shutdown( get_unix_fd( server->pipe_end.fd ), SHUT_RDWR );
    release_object( server->pipe_end.fd );
    server->pipe_end.fd = NULL;
}

static void pipe_end_destroy( struct pipe_end *pipe_end )
{
    struct pipe_message *message;

    while (!list_empty( &pipe_end->message_queue ))
    {
        message = LIST_ENTRY( list_head(&pipe_end->message_queue), struct pipe_message, entry );
        assert( !message->async );
        free_message( message );
    }

    free_async_queue( pipe_end->read_q );
    free_async_queue( pipe_end->write_q );
}

static void pipe_server_destroy( struct object *obj)
{
    struct pipe_server *server = (struct pipe_server *)obj;

    assert( obj->ops == &pipe_server_ops );

    pipe_end_disconnect( &server->pipe_end, STATUS_PIPE_BROKEN );

    if (server->pipe_end.fd)
    {
        notify_empty( server );
        do_disconnect( server );
    }

    pipe_end_destroy( &server->pipe_end );
    if (server->client)
    {
        server->client->server = NULL;
        server->client = NULL;
    }

    assert( server->pipe->instances );
    server->pipe->instances--;

    if (server->ioctl_fd) release_object( server->ioctl_fd );
    list_remove( &server->entry );
    release_object( server->pipe );
}

static void pipe_client_destroy( struct object *obj)
{
    struct pipe_client *client = (struct pipe_client *)obj;
    struct pipe_server *server = client->server;

    assert( obj->ops == &pipe_client_ops );

    pipe_end_disconnect( &client->pipe_end, STATUS_PIPE_BROKEN );

    if (server)
    {
        notify_empty( server );

        switch(server->state)
        {
        case ps_connected_server:
            /* Don't destroy the server's fd here as we can't
               do a successful flush without it. */
            set_server_state( server, ps_wait_disconnect );
            break;
        case ps_idle_server:
        case ps_wait_open:
        case ps_wait_disconnect:
        case ps_wait_connect:
            assert( 0 );
        }
        assert( server->client );
        server->client = NULL;
        client->server = NULL;
    }

    pipe_end_destroy( &client->pipe_end );
    if (client->pipe_end.fd) release_object( client->pipe_end.fd );
}

static void named_pipe_device_dump( struct object *obj, int verbose )
{
    fputs( "Named pipe device\n", stderr );
}

static struct object_type *named_pipe_device_get_type( struct object *obj )
{
    static const WCHAR name[] = {'D','e','v','i','c','e'};
    static const struct unicode_str str = { name, sizeof(name) };
    return get_object_type( &str );
}

static struct fd *named_pipe_device_get_fd( struct object *obj )
{
    struct named_pipe_device *device = (struct named_pipe_device *)obj;
    return (struct fd *)grab_object( device->fd );
}

static struct object *named_pipe_device_lookup_name( struct object *obj, struct unicode_str *name,
                                                     unsigned int attr )
{
    struct named_pipe_device *device = (struct named_pipe_device*)obj;
    struct object *found;

    assert( obj->ops == &named_pipe_device_ops );
    assert( device->pipes );

    if (!name) return NULL;  /* open the device itself */

    if ((found = find_object( device->pipes, name, attr | OBJ_CASE_INSENSITIVE )))
        name->len = 0;

    return found;
}

static struct object *named_pipe_device_open_file( struct object *obj, unsigned int access,
                                                   unsigned int sharing, unsigned int options )
{
    return grab_object( obj );
}

static void named_pipe_device_destroy( struct object *obj )
{
    struct named_pipe_device *device = (struct named_pipe_device*)obj;
    assert( obj->ops == &named_pipe_device_ops );
    if (device->fd) release_object( device->fd );
    free( device->pipes );
}

static enum server_fd_type named_pipe_device_get_fd_type( struct fd *fd )
{
    return FD_TYPE_DEVICE;
}

struct object *create_named_pipe_device( struct object *root, const struct unicode_str *name )
{
    struct named_pipe_device *dev;

    if ((dev = create_named_object( root, &named_pipe_device_ops, name, 0, NULL )) &&
        get_error() != STATUS_OBJECT_NAME_EXISTS)
    {
        dev->pipes = NULL;
        if (!(dev->fd = alloc_pseudo_fd( &named_pipe_device_fd_ops, &dev->obj, 0 )) ||
            !(dev->pipes = create_namespace( 7 )))
        {
            release_object( dev );
            dev = NULL;
        }
    }
    return &dev->obj;
}

static int pipe_data_remaining( struct pipe_server *server )
{
    struct pollfd pfd;
    int fd;

    assert( server->client );

    if (use_server_io( &server->pipe_end ))
        return !list_empty( &server->client->pipe_end.message_queue );

    fd = get_unix_fd( server->client->pipe_end.fd );
    if (fd < 0)
        return 0;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    if (0 > poll( &pfd, 1, 0 ))
        return 0;
 
    return pfd.revents&POLLIN;
}

static void check_flushed( void *arg )
{
    struct pipe_server *server = (struct pipe_server*) arg;

    if (pipe_data_remaining( server ))
    {
        server->flush_poll = add_timeout_user( -TICKS_PER_SEC / 10, check_flushed, server );
    }
    else
    {
        server->flush_poll = NULL;
        fd_async_wake_up( server->pipe_end.fd, ASYNC_TYPE_WAIT, STATUS_SUCCESS );
    }
}

static obj_handle_t pipe_end_flush( struct pipe_end *pipe_end, struct async *async )
{
    obj_handle_t handle = 0;

    if (use_server_io( pipe_end ) && (!pipe_end->connection || list_empty( &pipe_end->connection->message_queue )))
        return 0;

    if (!fd_queue_async( pipe_end->fd, async, ASYNC_TYPE_WAIT )) return 0;

    if (!async_is_blocking( async ) || (handle = alloc_handle( current->process, async, SYNCHRONIZE, 0 )))
        set_error( STATUS_PENDING );
    return handle;
}

static obj_handle_t pipe_server_flush( struct fd *fd, struct async *async )
{
    struct pipe_server *server = get_fd_user( fd );
    obj_handle_t handle;

    if (!server || server->state != ps_connected_server) return 0;

    if (!pipe_data_remaining( server )) return 0;

    handle = pipe_end_flush( &server->pipe_end, async );

    /* there's no unix way to be alerted when a pipe becomes empty, so resort to polling */
    if (handle && !use_server_io( &server->pipe_end ) && !server->flush_poll)
        server->flush_poll = add_timeout_user( -TICKS_PER_SEC / 10, check_flushed, server );
    return handle;
}

static obj_handle_t pipe_client_flush( struct fd *fd, struct async *async )
{
    struct pipe_end *pipe_end = get_fd_user( fd );
    /* FIXME: Support byte mode. */
    return use_server_io( pipe_end ) ? pipe_end_flush( pipe_end, async ) : 0;
}

static void message_queue_read( struct pipe_end *pipe_end, struct iosb *iosb )
{
    struct pipe_message *message;

    if (pipe_end->flags & NAMED_PIPE_MESSAGE_STREAM_READ)
    {
        message = LIST_ENTRY( list_head(&pipe_end->message_queue), struct pipe_message, entry );
        iosb->out_size = min( iosb->out_size, message->iosb->in_size - message->read_pos );
        iosb->status = message->read_pos + iosb->out_size < message->iosb->in_size
            ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
    else
    {
        data_size_t avail = 0;
        LIST_FOR_EACH_ENTRY( message, &pipe_end->message_queue, struct pipe_message, entry )
        {
            avail += message->iosb->in_size - message->read_pos;
            if (avail >= iosb->out_size) break;
        }
        iosb->out_size = min( iosb->out_size, avail );
        iosb->status = STATUS_SUCCESS;
    }

    message = LIST_ENTRY( list_head(&pipe_end->message_queue), struct pipe_message, entry );
    if (!message->read_pos && message->iosb->in_size == iosb->out_size) /* fast path */
    {
        iosb->out_data = message->iosb->in_data;
        message->iosb->in_data = NULL;
        wake_message( message );
        free_message( message );
    }
    else
    {
        data_size_t write_pos = 0, writing;
        char *buf = NULL;

        if (iosb->out_size && !(buf = iosb->out_data = malloc( iosb->out_size )))
        {
            iosb->out_size = 0;
            iosb->status = STATUS_NO_MEMORY;
            return;
        }

        do
        {
            message = LIST_ENTRY( list_head(&pipe_end->message_queue), struct pipe_message, entry );
            writing = min( iosb->out_size - write_pos, message->iosb->in_size - message->read_pos );
            if (writing) memcpy( buf + write_pos, (const char *)message->iosb->in_data + message->read_pos, writing );
            write_pos += writing;
            message->read_pos += writing;
            if (message->read_pos == message->iosb->in_size)
            {
                wake_message(message);
                free_message(message);
            }
        } while (write_pos < iosb->out_size);
    }
    iosb->result = iosb->out_size;
}

/* We call async_terminate in our reselect implementation, which causes recursive reselect.
 * We're not interested in such reselect calls, so we ignore them. */
static int ignore_reselect;

static void reselect_write_queue( struct pipe_end *pipe_end );

static void reselect_read_queue( struct pipe_end *pipe_end )
{
    struct async *async;
    struct iosb *iosb;
    int read_done = 0;

    ignore_reselect = 1;
    while (!list_empty( &pipe_end->message_queue) && (async = find_pending_async( pipe_end->read_q )))
    {
        iosb = async_get_iosb( async );
        message_queue_read( pipe_end, iosb );
        async_terminate( async, iosb->result ? STATUS_ALERTED : iosb->status );
        release_object( async );
        release_object( iosb );
        read_done = 1;
    }
    ignore_reselect = 0;

    if (pipe_end->connection)
    {
        if (list_empty( &pipe_end->message_queue ))
            fd_async_wake_up( pipe_end->connection->fd, ASYNC_TYPE_WAIT, STATUS_SUCCESS );
        else if (read_done)
            reselect_write_queue( pipe_end->connection );
    }
}

static void reselect_write_queue( struct pipe_end *pipe_end )
{
    struct pipe_message *message, *next;
    struct pipe_end *reader = pipe_end->connection;
    data_size_t avail = 0;

    if (!reader) return;

    ignore_reselect = 1;

    LIST_FOR_EACH_ENTRY_SAFE( message, next, &reader->message_queue, struct pipe_message, entry )
    {
        if (message->async && message->iosb->status != STATUS_PENDING)
        {
            release_object( message->async );
            message->async = NULL;
            free_message( message );
        }
        else
        {
            avail += message->iosb->in_size - message->read_pos;
            if (message->iosb->status == STATUS_PENDING && (avail <= reader->buffer_size || !message->iosb->in_size))
                wake_message( message );
        }
    }

    ignore_reselect = 0;
    reselect_read_queue( reader );
}

static obj_handle_t pipe_end_read( struct fd *fd, struct async *async, file_pos_t pos )
{
    struct pipe_end *pipe_end = get_fd_user( fd );
    obj_handle_t handle = 0;

    if (!use_server_io( pipe_end )) return no_fd_read( fd, async, pos );

    if (!pipe_end->connection && list_empty( &pipe_end->message_queue ))
    {
        set_error( STATUS_PIPE_BROKEN );
        return 0;
    }

    if (!pipe_end->read_q && !(pipe_end->read_q = create_async_queue( fd ))) return 0;
    if (!(handle = alloc_handle( current->process, async, SYNCHRONIZE, 0 ))) return 0;

    queue_async( pipe_end->read_q, async );
    reselect_read_queue( pipe_end );
    set_error( STATUS_PENDING );

    if (!async_is_blocking( async ))
    {
        struct iosb *iosb;
        iosb = async_get_iosb( async );
        if (iosb->status == STATUS_PENDING)
        {
            close_handle( current->process, handle );
            handle = 0;
        }
        release_object( iosb );
    }
    return handle;
}

static obj_handle_t pipe_end_write( struct fd *fd, struct async *async, file_pos_t pos )
{
    struct pipe_end *write_end = get_fd_user( fd );
    struct pipe_end *read_end = write_end->connection;
    struct pipe_message *message;
    obj_handle_t handle = 0;

    if (!use_server_io( write_end )) return no_fd_write( fd, async, pos );

    if (!read_end)
    {
        set_error( STATUS_PIPE_DISCONNECTED );
        return 0;
    }

    if (!write_end->write_q && !(write_end->write_q = create_async_queue( fd ))) return 0;
    if (!(handle = alloc_handle( current->process, async, SYNCHRONIZE, 0 ))) return 0;

    if (!(message = mem_alloc( sizeof(*message) )))
    {
        close_handle( current->process, handle );
        return 0;
    }
    message->async = (struct async *)grab_object( async );
    message->iosb = async_get_iosb( async );
    message->read_pos = 0;
    list_add_tail( &read_end->message_queue, &message->entry );

    queue_async( write_end->write_q, async );
    reselect_write_queue( write_end );
    set_error( STATUS_PENDING );

    if (!async_is_blocking( async ))
    {
        struct iosb *iosb;
        iosb = async_get_iosb( async );
        if (iosb->status == STATUS_PENDING)
        {
            close_handle( current->process, handle );
            handle = 0;
        }
        release_object( iosb );
    }
    return handle;
}

static void pipe_end_queue_async( struct fd *fd, struct async *async, int type, int count )
{
    struct pipe_end *pipe_end = get_fd_user( fd );
    if (use_server_io( pipe_end )) no_fd_queue_async( fd, async, type, count );
    else default_fd_queue_async( fd, async, type, count );
}

static void pipe_end_reselect_async( struct fd *fd, struct async_queue *queue )
{
    struct pipe_end *pipe_end = get_fd_user( fd );

    if (ignore_reselect) return;

    if (!use_server_io( pipe_end ))
        default_fd_reselect_async( fd, queue );
    else if (pipe_end->write_q && pipe_end->write_q == queue)
        reselect_write_queue( pipe_end );
    else if (pipe_end->read_q && pipe_end->read_q == queue)
        reselect_read_queue( pipe_end );
}

static inline int is_overlapped( unsigned int options )
{
    return !(options & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT));
}

static enum server_fd_type pipe_end_get_fd_type( struct fd *fd )
{
    return FD_TYPE_PIPE;
}

static void pipe_end_peek( struct pipe_end *pipe_end )
{
    unsigned reply_size = get_reply_max_size();
    FILE_PIPE_PEEK_BUFFER *buffer;
    struct pipe_message *message;
    data_size_t avail = 0;
    data_size_t message_length = 0;

    if (!use_server_io( pipe_end ))
    {
        set_error( STATUS_NOT_SUPPORTED );
        return;
    }

    if (reply_size < offsetof( FILE_PIPE_PEEK_BUFFER, Data ))
    {
        set_error( STATUS_INFO_LENGTH_MISMATCH );
        return;
    }
    reply_size -= offsetof( FILE_PIPE_PEEK_BUFFER, Data );

    LIST_FOR_EACH_ENTRY( message, &pipe_end->message_queue, struct pipe_message, entry )
        avail += message->iosb->in_size - message->read_pos;

    if (avail)
    {
        message = LIST_ENTRY( list_head(&pipe_end->message_queue), struct pipe_message, entry );
        message_length = message->iosb->in_size - message->read_pos;
        reply_size = min( reply_size, message_length );
    }
    else reply_size = 0;

    if (!(buffer = set_reply_data_size( offsetof( FILE_PIPE_PEEK_BUFFER, Data[reply_size] )))) return;
    buffer->NamedPipeState    = 0;  /* FIXME */
    buffer->ReadDataAvailable = avail;
    buffer->NumberOfMessages  = 0;  /* FIXME */
    buffer->MessageLength     = message_length;
    if (reply_size) memcpy( buffer->Data, (const char *)message->iosb->in_data + message->read_pos, reply_size );
}

static obj_handle_t pipe_server_ioctl( struct fd *fd, ioctl_code_t code, struct async *async )
{
    struct pipe_server *server = get_fd_user( fd );
    obj_handle_t wait_handle = 0;

    switch(code)
    {
    case FSCTL_PIPE_LISTEN:
        switch(server->state)
        {
        case ps_idle_server:
        case ps_wait_connect:
            if (fd_queue_async( server->ioctl_fd, async, ASYNC_TYPE_WAIT ))
            {
                if (async_is_blocking( async )) wait_handle = alloc_handle( current->process, async, SYNCHRONIZE, 0 );
                set_server_state( server, ps_wait_open );
                if (server->pipe->waiters) async_wake_up( server->pipe->waiters, STATUS_SUCCESS );
                set_error( STATUS_PENDING );
                return wait_handle;
            }
            break;
        case ps_connected_server:
            set_error( STATUS_PIPE_CONNECTED );
            break;
        case ps_wait_disconnect:
            set_error( STATUS_NO_DATA_DETECTED );
            break;
        case ps_wait_open:
            set_error( STATUS_INVALID_HANDLE );
            break;
        }
        return 0;

    case FSCTL_PIPE_DISCONNECT:
        switch(server->state)
        {
        case ps_connected_server:
            assert( server->client );
            assert( server->client->pipe_end.fd );

            notify_empty( server );

            /* dump the client and server fds - client loses all waiting data */
            pipe_end_disconnect( &server->pipe_end, STATUS_PIPE_DISCONNECTED );
            do_disconnect( server );
            server->client->server = NULL;
            server->client = NULL;
            set_server_state( server, ps_wait_connect );
            break;
        case ps_wait_disconnect:
            assert( !server->client );
            pipe_end_disconnect( &server->pipe_end, STATUS_PIPE_DISCONNECTED );
            do_disconnect( server );
            set_server_state( server, ps_wait_connect );
            break;
        case ps_idle_server:
        case ps_wait_open:
            set_error( STATUS_PIPE_LISTENING );
            break;
        case ps_wait_connect:
            set_error( STATUS_PIPE_DISCONNECTED );
            break;
        }
        return 0;

    case FSCTL_PIPE_PEEK:
        pipe_end_peek( &server->pipe_end );
        return 0;

    default:
        return default_fd_ioctl( fd, code, async );
    }
}

static obj_handle_t pipe_client_ioctl( struct fd *fd, ioctl_code_t code, struct async *async )
{
    struct pipe_client *client = get_fd_user( fd );

    switch(code)
    {
    case FSCTL_PIPE_PEEK:
        pipe_end_peek( &client->pipe_end );
        return 0;

    default:
        return default_fd_ioctl( fd, code, async );
    }
}

static struct pipe_server *get_pipe_server_obj( struct process *process,
                                obj_handle_t handle, unsigned int access )
{
    struct object *obj;
    obj = get_handle_obj( process, handle, access, &pipe_server_ops );
    return (struct pipe_server *) obj;
}

static void init_pipe_end( struct pipe_end *pipe_end, unsigned int pipe_flags, data_size_t buffer_size )
{
    pipe_end->fd = NULL;
    pipe_end->flags = pipe_flags;
    pipe_end->connection = NULL;
    pipe_end->buffer_size = buffer_size;
    pipe_end->read_q = NULL;
    pipe_end->write_q = NULL;
    list_init( &pipe_end->message_queue );
}

static struct pipe_server *create_pipe_server( struct named_pipe *pipe, unsigned int options,
                                               unsigned int pipe_flags )
{
    struct pipe_server *server;

    server = alloc_object( &pipe_server_ops );
    if (!server)
        return NULL;

    server->pipe = pipe;
    server->client = NULL;
    server->flush_poll = NULL;
    server->options = options;
    init_pipe_end( &server->pipe_end, pipe_flags, pipe->insize );

    list_add_head( &pipe->servers, &server->entry );
    grab_object( pipe );
    if (!(server->ioctl_fd = alloc_pseudo_fd( &pipe_server_fd_ops, &server->pipe_end.obj, options )))
    {
        release_object( server );
        return NULL;
    }
    set_fd_signaled( server->ioctl_fd, 1 );
    set_server_state( server, ps_idle_server );
    return server;
}

static struct pipe_client *create_pipe_client( unsigned int flags, unsigned int pipe_flags, data_size_t buffer_size )
{
    struct pipe_client *client;

    client = alloc_object( &pipe_client_ops );
    if (!client)
        return NULL;

    client->server = NULL;
    client->flags = flags;
    init_pipe_end( &client->pipe_end, pipe_flags, buffer_size );

    return client;
}

static struct pipe_server *find_available_server( struct named_pipe *pipe )
{
    struct pipe_server *server;

    /* look for pipe servers that are listening */
    LIST_FOR_EACH_ENTRY( server, &pipe->servers, struct pipe_server, entry )
    {
        if (server->state == ps_wait_open)
            return (struct pipe_server *)grab_object( server );
    }

    /* fall back to pipe servers that are idle */
    LIST_FOR_EACH_ENTRY( server, &pipe->servers, struct pipe_server, entry )
    {
        if (server->state == ps_idle_server)
            return (struct pipe_server *)grab_object( server );
    }

    return NULL;
}

static int named_pipe_link_name( struct object *obj, struct object_name *name, struct object *parent )
{
    struct named_pipe_device *dev = (struct named_pipe_device *)parent;

    if (parent->ops != &named_pipe_device_ops)
    {
        set_error( STATUS_OBJECT_NAME_INVALID );
        return 0;
    }
    namespace_add( dev->pipes, name );
    name->parent = grab_object( parent );
    return 1;
}

static struct object *named_pipe_open_file( struct object *obj, unsigned int access,
                                            unsigned int sharing, unsigned int options )
{
    struct named_pipe *pipe = (struct named_pipe *)obj;
    struct pipe_server *server;
    struct pipe_client *client;
    unsigned int pipe_sharing;
    int fds[2];

    if (!(server = find_available_server( pipe )))
    {
        set_error( STATUS_PIPE_NOT_AVAILABLE );
        return NULL;
    }

    pipe_sharing = server->pipe->sharing;
    if (((access & GENERIC_READ) && !(pipe_sharing & FILE_SHARE_READ)) ||
        ((access & GENERIC_WRITE) && !(pipe_sharing & FILE_SHARE_WRITE)))
    {
        set_error( STATUS_ACCESS_DENIED );
        release_object( server );
        return NULL;
    }

    if ((client = create_pipe_client( options, pipe->flags, pipe->outsize )))
    {
        if (use_server_io( &server->pipe_end ))
        {
            client->pipe_end.fd = alloc_pseudo_fd( &pipe_client_fd_ops, &client->pipe_end.obj, options );
            if (client->pipe_end.fd)
            {
                set_fd_signaled( client->pipe_end.fd, 1 );
                server->pipe_end.fd = (struct fd *)grab_object( server->ioctl_fd );
                set_no_fd_status( server->ioctl_fd, STATUS_BAD_DEVICE_TYPE );
            }
            else
            {
                release_object( client );
                client = NULL;
            }
        }
        else if (!socketpair( PF_UNIX, SOCK_STREAM, 0, fds ))
        {
            assert( !server->pipe_end.fd );

            /* for performance reasons, only set nonblocking mode when using
             * overlapped I/O. Otherwise, we will be doing too much busy
             * looping */
            if (is_overlapped( options )) fcntl( fds[1], F_SETFL, O_NONBLOCK );
            if (is_overlapped( server->options )) fcntl( fds[0], F_SETFL, O_NONBLOCK );

            if (pipe->insize)
            {
                setsockopt( fds[0], SOL_SOCKET, SO_RCVBUF, &pipe->insize, sizeof(pipe->insize) );
                setsockopt( fds[1], SOL_SOCKET, SO_RCVBUF, &pipe->insize, sizeof(pipe->insize) );
            }
            if (pipe->outsize)
            {
                setsockopt( fds[0], SOL_SOCKET, SO_SNDBUF, &pipe->outsize, sizeof(pipe->outsize) );
                setsockopt( fds[1], SOL_SOCKET, SO_SNDBUF, &pipe->outsize, sizeof(pipe->outsize) );
            }

            client->pipe_end.fd = create_anonymous_fd( &pipe_client_fd_ops, fds[1], &client->pipe_end.obj, options );
            server->pipe_end.fd = create_anonymous_fd( &pipe_server_fd_ops, fds[0], &server->pipe_end.obj, server->options );
            if (client->pipe_end.fd && server->pipe_end.fd)
            {
                fd_copy_completion( server->ioctl_fd, server->pipe_end.fd );
            }
            else
            {
                release_object( client );
                client = NULL;
            }
        }
        else
        {
            file_set_error();
            release_object( client );
            client = NULL;
        }
        if (client)
        {
            allow_fd_caching( client->pipe_end.fd );
            allow_fd_caching( server->pipe_end.fd );
            if (server->state == ps_wait_open)
                fd_async_wake_up( server->ioctl_fd, ASYNC_TYPE_WAIT, STATUS_SUCCESS );
            set_server_state( server, ps_connected_server );
            server->client = client;
            client->server = server;
            server->pipe_end.connection = &client->pipe_end;
            client->pipe_end.connection = &server->pipe_end;
        }
    }
    release_object( server );
    return &client->pipe_end.obj;
}

static obj_handle_t named_pipe_device_ioctl( struct fd *fd, ioctl_code_t code, struct async *async )
{
    struct named_pipe_device *device = get_fd_user( fd );

    switch(code)
    {
    case FSCTL_PIPE_WAIT:
        {
            const FILE_PIPE_WAIT_FOR_BUFFER *buffer = get_req_data();
            data_size_t size = get_req_data_size();
            obj_handle_t wait_handle = 0;
            struct named_pipe *pipe;
            struct pipe_server *server;
            struct unicode_str name;
            timeout_t when;

            if (size < sizeof(*buffer) ||
                size < FIELD_OFFSET(FILE_PIPE_WAIT_FOR_BUFFER, Name[buffer->NameLength/sizeof(WCHAR)]))
            {
                set_error( STATUS_INVALID_PARAMETER );
                return 0;
            }
            name.str = buffer->Name;
            name.len = (buffer->NameLength / sizeof(WCHAR)) * sizeof(WCHAR);
            if (!(pipe = open_named_object( &device->obj, &named_pipe_ops, &name, 0 ))) return 0;

            if (!(server = find_available_server( pipe )))
            {
                if (!pipe->waiters && !(pipe->waiters = create_async_queue( NULL ))) goto done;

                queue_async( pipe->waiters, async );
                when = buffer->TimeoutSpecified ? buffer->Timeout.QuadPart : pipe->timeout;
                async_set_timeout( async, when, STATUS_IO_TIMEOUT );
                if (async_is_blocking( async )) wait_handle = alloc_handle( current->process, async, SYNCHRONIZE, 0 );
                set_error( STATUS_PENDING );
            }
            else release_object( server );

        done:
            release_object( pipe );
            return wait_handle;
        }

    default:
        return default_fd_ioctl( fd, code, async );
    }
}


DECL_HANDLER(create_named_pipe)
{
    struct named_pipe *pipe;
    struct pipe_server *server;
    struct unicode_str name;
    struct object *root;
    const struct security_descriptor *sd;
    const struct object_attributes *objattr = get_req_object_attributes( &sd, &name, &root );

    if (!objattr) return;

    if (!req->sharing || (req->sharing & ~(FILE_SHARE_READ | FILE_SHARE_WRITE)) ||
        (!(req->flags & NAMED_PIPE_MESSAGE_STREAM_WRITE) && (req->flags & NAMED_PIPE_MESSAGE_STREAM_READ)))
    {
        if (root) release_object( root );
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if (!name.len)  /* pipes need a root directory even without a name */
    {
        if (!objattr->rootdir)
        {
            set_error( STATUS_OBJECT_PATH_SYNTAX_BAD );
            return;
        }
        if (!(root = get_directory_obj( current->process, objattr->rootdir ))) return;
    }

    pipe = create_named_object( root, &named_pipe_ops, &name, objattr->attributes | OBJ_OPENIF, NULL );

    if (root) release_object( root );
    if (!pipe) return;

    if (get_error() != STATUS_OBJECT_NAME_EXISTS)
    {
        /* initialize it if it didn't already exist */
        pipe->instances = 0;
        pipe->waiters = NULL;
        list_init( &pipe->servers );
        pipe->insize = req->insize;
        pipe->outsize = req->outsize;
        pipe->maxinstances = req->maxinstances;
        pipe->timeout = req->timeout;
        pipe->flags = req->flags & NAMED_PIPE_MESSAGE_STREAM_WRITE;
        pipe->sharing = req->sharing;
    }
    else
    {
        if (pipe->maxinstances <= pipe->instances)
        {
            set_error( STATUS_INSTANCE_NOT_AVAILABLE );
            release_object( pipe );
            return;
        }
        if (pipe->sharing != req->sharing)
        {
            set_error( STATUS_ACCESS_DENIED );
            release_object( pipe );
            return;
        }
        clear_error(); /* clear the name collision */
    }

    server = create_pipe_server( pipe, req->options, req->flags );
    if (server)
    {
        reply->handle = alloc_handle( current->process, server, req->access, objattr->attributes );
        server->pipe->instances++;
        if (sd) default_set_sd( &server->pipe_end.obj, sd, OWNER_SECURITY_INFORMATION |
                                                           GROUP_SECURITY_INFORMATION |
                                                           DACL_SECURITY_INFORMATION |
                                                           SACL_SECURITY_INFORMATION );
        release_object( server );
    }

    release_object( pipe );
}

DECL_HANDLER(get_named_pipe_info)
{
    struct pipe_server *server;
    struct pipe_client *client = NULL;

    server = get_pipe_server_obj( current->process, req->handle, FILE_READ_ATTRIBUTES );
    if (!server)
    {
        if (get_error() != STATUS_OBJECT_TYPE_MISMATCH)
            return;

        clear_error();
        client = (struct pipe_client *)get_handle_obj( current->process, req->handle,
                                                       0, &pipe_client_ops );
        if (!client) return;
        server = client->server;
    }

    reply->flags = client ? client->pipe_end.flags : server->pipe_end.flags;
    if (server)
    {
        reply->sharing      = server->pipe->sharing;
        reply->maxinstances = server->pipe->maxinstances;
        reply->instances    = server->pipe->instances;
        reply->insize       = server->pipe->insize;
        reply->outsize      = server->pipe->outsize;
    }

    if (client)
        release_object(client);
    else
    {
        reply->flags |= NAMED_PIPE_SERVER_END;
        release_object(server);
    }
}

DECL_HANDLER(set_named_pipe_info)
{
    struct pipe_server *server;
    struct pipe_client *client = NULL;

    server = get_pipe_server_obj( current->process, req->handle, FILE_WRITE_ATTRIBUTES );
    if (!server)
    {
        if (get_error() != STATUS_OBJECT_TYPE_MISMATCH)
            return;

        clear_error();
        client = (struct pipe_client *)get_handle_obj( current->process, req->handle,
                                                       0, &pipe_client_ops );
        if (!client) return;
        if (!(server = client->server))
        {
            release_object( client );
            return;
        }
    }

    if ((req->flags & ~(NAMED_PIPE_MESSAGE_STREAM_READ | NAMED_PIPE_NONBLOCKING_MODE)) ||
            ((req->flags & NAMED_PIPE_MESSAGE_STREAM_READ) && !(server->pipe->flags & NAMED_PIPE_MESSAGE_STREAM_WRITE)))
    {
        set_error( STATUS_INVALID_PARAMETER );
    }
    else if (client)
    {
        client->pipe_end.flags = server->pipe->flags | req->flags;
    }
    else
    {
        server->pipe_end.flags = server->pipe->flags | req->flags;
    }

    if (client)
        release_object(client);
    else
        release_object(server);
}
