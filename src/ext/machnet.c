/**
 * @file  machnet.c
 * @brief All of the Machnet public API functions are implemented as inline in
 * `machnet.h' and `machnet_common.h'. The functions in this translation unit
 * are simple wrappers to generate a shared library with symbols for easier FFI
 * integration.
 */

#include "machnet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "machnet_ctrl.h"

#define MIN(a, b)           \
  ({                        \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    _a < _b ? _a : _b;      \
  })

// Main socket/connection to the Machnet controller.
int g_ctrl_socket = -1;

// Application UUID.
uuid_t g_app_uuid;
char g_app_uuid_str[37];

// Monotonically increasing counter for generating unique IDs.
static uint32_t msg_id_counter;

/**
 * @brief Helper function to issue control requests to the Machnet controller.
 * @param req  Pointer to the request message (will be sent to the controller).
 * @param resp Pointer to the response message buffer; response will be copied
 * there.
 * @param fd   Pointer to the file descriptor location (provided by the caller)
 * ; if the response message carries a file descriptor.
 * @return 0 on success.
 * @attention The caller is responsible for allocating the request and response
 * buffers. This function is thread-safe.
 */
static int _machnet_ctrl_request(machnet_ctrl_msg_t *req,
                                 machnet_ctrl_msg_t *resp, int *fd) {
  // We do maintain a global socket to the controller for the duration of the
  // application's lifetime, but we rather open a new connection to the
  // controller for each request. The reason for this is to achieve thread
  // safety (multiple application threads issuing concurrent requests to the
  // controller).

  // Connect to the local AF_UNIX domain socket.
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    return -1;
  }

  struct sockaddr_un server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, MACHNET_CONTROLLER_DEFAULT_PATH,
          sizeof(server_addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("connect");
    return -1;
  }

  // Send the request to the controller.
  struct iovec iov[1];
  iov[0].iov_base = req;
  iov[0].iov_len = sizeof(*req);
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;

  int nbytes = sendmsg(sock, &msg, 0);
  if (nbytes != sizeof(*req)) {
    // We got an error or a partial transmission.
    if (nbytes < 0) {
      perror("sendmsg");
    }
    return -1;
  }

  // Block waiting for the response using recvmsg.
  iov[0].iov_base = resp;
  iov[0].iov_len = sizeof(*resp);
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  // We need to allocate a buffer for the ancillary data.
  char buf[CMSG_SPACE(sizeof(int))];
  memset(buf, 0, sizeof(buf));
  msg.msg_control = buf;
  msg.msg_controllen = sizeof(buf);

  nbytes = recvmsg(sock, &msg, 0);
  if (nbytes != sizeof(*resp)) {
    // We got an error or a partial response.
    if (nbytes < 0) {
      perror("recvmsg");
    }
    return -1;
  }

  if (fd != NULL) {
    *fd = -1;
    fprintf(stderr, "Checking for file descriptor...\n");
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg != NULL && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS) {
      fprintf(stderr, "Got a file descriptor!\n");
      assert(cmsg->cmsg_len == CMSG_LEN(sizeof(int)));
      // We got a file descriptor.
      *fd = *((int *)CMSG_DATA(cmsg));
    }
  }

  return 0;
}

int machnet_init() {
  uuid_t zero_uuid;
  uuid_clear(zero_uuid);
  if (uuid_compare(zero_uuid, g_app_uuid) != 0) {
    // Already initialized.
    return 0;
  }

  // Generate a random UUID for this application.
  uuid_generate(g_app_uuid);
  uuid_unparse(g_app_uuid, g_app_uuid_str);

  // Initialize the AF_UNIX socket to the controller.
  g_ctrl_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (g_ctrl_socket < 0) {
    return -1;
  }

  // Connect to the controller.
  struct sockaddr_un server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, MACHNET_CONTROLLER_DEFAULT_PATH,
          sizeof(server_addr.sun_path) - 1);
  if (connect(g_ctrl_socket, (struct sockaddr *)&server_addr,
              sizeof(server_addr)) < 0) {
    fprintf(stderr,
            "ERROR: Failed to connect() to the Machnet controller at %s\n",
            MACHNET_CONTROLLER_DEFAULT_PATH);
    return -1;
  }

  // Send REGISTER message.
  machnet_ctrl_msg_t req = {.type = MACHNET_CTRL_MSG_TYPE_REQ_REGISTER,
                            .msg_id = msg_id_counter++};
  uuid_copy(req.app_uuid, g_app_uuid);
  machnet_ctrl_msg_t resp = {};

  // Sendmsg request.
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  struct iovec iov[1];
  iov[0].iov_base = &req;
  iov[0].iov_len = sizeof(req);
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  int nbytes = sendmsg(g_ctrl_socket, &msg, 0);
  if (nbytes < 0) {
    fprintf(stderr, "ERROR: Failed to send register message to controller.\n");
    perror("sendmsg(): ");
    return -1;
  }

  // Recvmsg response.
  iov[0].iov_base = &resp;
  iov[0].iov_len = sizeof(resp);
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  nbytes = recvmsg(g_ctrl_socket, &msg, 0);
  if (nbytes < 0 || nbytes != sizeof(resp)) {
    fprintf(stderr, "Got invalid response from controller.\n");
    return -1;
  }

  // Check the response.
  if (resp.type != MACHNET_CTRL_MSG_TYPE_RESPONSE ||
      resp.msg_id != req.msg_id) {
    fprintf(stderr, "Got invalid response from controller.\n");
    return -1;
  }

  // It is important that we do not close the socket here. Closing the socket
  // will trigger the controller to de-register the application and release all
  // its allocated resources (shared memory channels, connections etc.). When
  // this application quits, the controller will detect that the socket was
  // closed and de-register the application.

  return resp.status;
}

MachnetChannelCtx_t *machnet_bind(int shm_fd, size_t *channel_size) {
  MachnetChannelCtx_t *channel;
  int shm_flags;
  if (channel_size != NULL) *channel_size = 0;

  // Check whether the shmem fd is open.
  if (fcntl(shm_fd, F_GETFD) == -1) {
    fprintf(stderr, "Invalid shared memory file descriptor: %d", shm_fd);
    goto fail;
  }

  // Get the size of the shared memory segment.
  struct stat stat_buf;
  if (fstat(shm_fd, &stat_buf) == -1) {
    perror("fstat()");
    goto fail;
  }

  // Map the shared memory segment into the address space of the process.
  shm_flags = MAP_SHARED | MAP_POPULATE;
  if (stat_buf.st_blksize > getpagesize()) {
    /* TODO(ilias): Hack to detect if mapping is huge page backed. */
    shm_flags |= MAP_HUGETLB;
  }
  channel = (MachnetChannelCtx_t *)mmap(
      NULL, stat_buf.st_size, PROT_READ | PROT_WRITE, shm_flags, shm_fd, 0);
  if (channel == MAP_FAILED) {
    perror("mmap()");
    goto fail;
  }

  if (channel->magic != MACHNET_CHANNEL_CTX_MAGIC) {
    fprintf(stderr, "Invalid magic number: %u\n", channel->magic);
    goto fail;
  }

  // Success.
  if (channel_size != NULL) *channel_size = stat_buf.st_size;

  return channel;

fail:
  if (shm_fd > 0) close(shm_fd);
  return NULL;
}

void *machnet_attach() {
  uuid_t uuid;        // UUID for the shared memory channel.
  char uuid_str[37];  // 36 chars + null terminator for UUID string.

  uuid_generate(uuid);
  uuid_unparse(uuid, uuid_str);

  // Generate a request to attach to the Machnet control plane.
  machnet_ctrl_msg_t req = {};
  req.type = MACHNET_CTRL_MSG_TYPE_REQ_CHANNEL;
  req.msg_id = msg_id_counter++;
  uuid_copy(req.app_uuid, g_app_uuid);
  uuid_copy(req.channel_info.channel_uuid, uuid);
  /* Request the default. */
  req.channel_info.desc_ring_size = MACHNET_CHANNEL_INFO_DESC_RING_SIZE_DEFAULT;
  req.channel_info.buffer_count = MACHNET_CHANNEL_INFO_BUFFER_COUNT_DEFAULT;

  // Send the request to the Machnet control plane.
  int channel_fd;
  machnet_ctrl_msg_t resp;
  if (_machnet_ctrl_request(&req, &resp, &channel_fd) != 0) {
    fprintf(stderr, "ERROR: Failed to send request to controller.");
    return NULL;
  }

  // Check the response from the Machnet control plane.
  if (resp.type != MACHNET_CTRL_MSG_TYPE_RESPONSE ||
      resp.msg_id != req.msg_id) {
    fprintf(stderr, "Got invalid response from controller.\n");
    return NULL;
  }

  if (resp.status != MACHNET_CTRL_STATUS_SUCCESS || channel_fd < 0) {
    fprintf(stderr, "Failure %d.\n", channel_fd);
    return NULL;
  }

  return machnet_bind(channel_fd, NULL);
}

int machnet_connect(void *channel_ctx, const char *src_ip, const char *dst_ip,
                    uint16_t dst_port, MachnetFlow_t *flow) {
  assert(flow != NULL);
  MachnetChannelCtx_t *ctx = channel_ctx;

  if (inet_addr(src_ip) == INADDR_NONE || inet_addr(dst_ip) == INADDR_ANY) {
    fprintf(stderr,
            "machnet_connect: Invalid source (%s) or destination (%s) "
            "IP address.\n",
            src_ip, dst_ip);
    return -1;
  }

  MachnetCtrlQueueEntry_t req;
  memset(&req, 0, sizeof(req));
  req.id = ctx->ctrl_ctx.req_id++;
  req.opcode = MACHNET_CTRL_OP_CREATE_FLOW;
  req.flow_info.src_ip = ntohl(inet_addr(src_ip));
  req.flow_info.dst_ip = ntohl(inet_addr(dst_ip));
  req.flow_info.dst_port = dst_port;

  // Send the request to the Machnet control plane.
  if (__machnet_channel_ctrl_sq_enqueue(ctx, 1, &req) != 1) {
    fprintf(stderr, "ERROR: Failed to enqueue request to control queue.\n");
    return -1;
  }

  MachnetCtrlQueueEntry_t resp;
  memset(&resp, 0, sizeof(resp));
  uint32_t ret = 0;
  int max_tries = 10;
  do {
    ret = __machnet_channel_ctrl_cq_dequeue(ctx, 1, &resp);
    if (ret != 0) break;
    sleep(1);
  } while (max_tries-- > 0);
  if (ret == 0) {
    fprintf(stderr, "ERROR: Failed to dequeue response from control queue.\n");
    return -1;
  }
  if (resp.id != req.id) {
    fprintf(stderr, "ERROR: Got invalid response from control plane.\n");
    return -1;
  }

  if (resp.status != MACHNET_CTRL_STATUS_OK) {
    fprintf(stderr, "ERROR: Got failure response from control plane.\n");
    return -1;
  }

  *flow = resp.flow_info;

  // Success.
  return 0;
}

int machnet_listen(void *channel_ctx, const char *local_ip,
                   uint16_t local_port) {
  assert(channel_ctx != NULL);
  MachnetChannelCtx_t *ctx = channel_ctx;

  if (inet_addr(local_ip) == INADDR_NONE) {
    fprintf(stderr, "machnet_listen: Invalid IP address: %s\n", local_ip);
    return -EINVAL;
  }

  MachnetCtrlQueueEntry_t req;
  memset(&req, 0, sizeof(req));
  req.id = ctx->ctrl_ctx.req_id++;
  req.opcode = MACHNET_CTRL_OP_LISTEN;
  req.listener_info.ip = ntohl(inet_addr(local_ip));
  req.listener_info.port = local_port;

  // Send the request to the Machnet control plane.
  if (__machnet_channel_ctrl_sq_enqueue(ctx, 1, &req) != 1) {
    fprintf(stderr, "ERROR: Failed to enqueue request to control queue.\n");
    return -1;
  }

  MachnetCtrlQueueEntry_t resp;
  memset(&resp, 0, sizeof(resp));
  uint32_t ret = 0;
  int max_tries = 10;
  do {
    ret = __machnet_channel_ctrl_cq_dequeue(ctx, 1, &resp);
    if (ret != 0) break;
    sleep(1);
  } while (max_tries-- > 0);
  if (ret == 0) {
    fprintf(stderr, "ERROR: Failed to dequeue response from control queue.\n");
    return -1;
  }
  if (resp.id != req.id) {
    fprintf(stderr, "ERROR: Got invalid response from control plane.\n");
    return -1;
  }

  if (resp.status != MACHNET_CTRL_STATUS_OK) {
    fprintf(stderr, "ERROR: Got failure response from control plane.\n");
    return -1;
  }

  // Success.
  return 0;
}

int machnet_send(const void *channel_ctx, MachnetFlow_t flow, const void *buf,
                 size_t len) {
  struct MachnetIovec iov;
  iov.base = (void *)buf;
  iov.len = len;

  struct MachnetMsgHdr msghdr;
  msghdr.flags = 0;
  msghdr.msg_size = len;
  msghdr.flow_info = flow;
  msghdr.msg_iov = &iov;
  msghdr.msg_iovlen = 1;

  return machnet_sendmsg(channel_ctx, &msghdr);
}

int machnet_sendmsg(const void *channel_ctx, const MachnetMsgHdr_t *msghdr) {
  assert(channel_ctx != NULL);
  assert(msghdr != NULL);
  const MachnetChannelCtx_t *ctx = (const MachnetChannelCtx_t *)channel_ctx;

  // Sanity checks on the full message size.
  if (unlikely(msghdr->msg_size > MACHNET_MSG_MAX_LEN || msghdr->msg_size == 0))
    return -1;

  // Get the maximum payload size of a message buffer.
  // This is dictated by the stack, during the channel creation.
  const uint32_t kMsgBufPayloadMax = ctx->data_ctx.buf_mss;

  // Calculate how many buffers we need to hold the message, and bulk allocate
  // them.
  //  uint32_t buffers_nr =
  //      (msghdr->msg_size + kMsgBufPayloadMax - 1) / kMsgBufPayloadMax;
  uint32_t buffers_nr = (msghdr->msg_size + MTU - 1) / MTU;
  //  fprintf(stderr,
  ////          "kMsgBufPayloadMax: %d msg_size: %d buffers_nr: %d "
  //          "msghdr->msg_iovlen: %zu\n",
  //          kMsgBufPayloadMax, msghdr->msg_size, buffers_nr,
  //          msghdr->msg_iovlen);
  MachnetRingSlot_t *buffer_indices =
      (MachnetRingSlot_t *)malloc(buffers_nr * sizeof(MachnetRingSlot_t));
  if (buffer_indices == NULL) return -1;
  // allocate buffers and init
  MachnetMsgBuf_t *buffers =
      (MachnetMsgBuf_t *)malloc(buffers_nr * sizeof(MachnetMsgBuf_t));

  for (size_t i = 0; i < buffers_nr; i++) {
    __machnet_channel_buf_init(&buffers[i]);
    *__DECONST(uint32_t *, &buffers[i].magic) = MACHNET_MSGBUF_MAGIC;
    *__DECONST(uint32_t *, &buffers[i].index) = i;
    *__DECONST(uint32_t *, &buffers[i].size) = MTU;
  }

  // enqueue buffers
  // Gather all message segments.
  uint32_t buffer_cur_index = 0;
  uint32_t total_bytes_copied = 0;
  for (size_t iov_index = 0; iov_index < msghdr->msg_iovlen; iov_index++) {
    const MachnetIovec_t *segment_desc = &msghdr->msg_iov[iov_index];
    assert(segment_desc != NULL);

    uchar_t *seg_data = (uchar_t *)segment_desc->base;
    uint32_t seg_bytes = segment_desc->len;
    fprintf(stderr, "seg_bytes: %d\n", seg_bytes);
    while (seg_bytes) {
      // Get the destination offset at buffer.
      MachnetMsgBuf_t buffer = buffers[buffer_cur_index];
      if (unlikely(buffer.magic != MACHNET_MSGBUF_MAGIC)) abort();

      // Copy the data.
      uint32_t nbytes_to_copy = MIN(seg_bytes, MTU);
      //      uchar_t *buf_data = __machnet_channel_buf_append(buffer,
      //      nbytes_to_copy);
      memcpy(buffer.data, seg_data, nbytes_to_copy);
      buffer.flags |= MACHNET_MSGBUF_FLAGS_SG;

      seg_data += nbytes_to_copy;
      seg_bytes -= nbytes_to_copy;
      total_bytes_copied += nbytes_to_copy;
      //      fprintf(stderr, "seg_bytes left: %d total_bytes_copied: %d\n",
      //      seg_bytes,
      //              total_bytes_copied);
      if (seg_bytes) {
        // The buffer is full, and we still have data to copy.
        buffer_cur_index++;  // Get the next buffer index.
                             //        fprintf(stderr,
        //                "buffer_cur_index: %d buffers_nr: %d seg_bytes: %d "
        //                "total_bytes_copied: %d\n",
        //                buffer_cur_index, buffers_nr, seg_bytes,
        //                total_bytes_copied);
        assert(buffer_cur_index <= buffers_nr);
        //        buffer->next =
        //            buffer_indices[buffer_cur_index];  // Link to the next
        //            buffer.
      }
    }
  }

  assert(total_bytes_copied == msghdr->msg_size);
  if (unlikely(total_bytes_copied != msghdr->msg_size)) abort();

  // For the last buffer, we need to mark it as the tail of the message.
  MachnetMsgBuf_t last = buffers[buffers_nr - 1];
  last.flags |= MACHNET_MSGBUF_FLAGS_FIN;
  last.flags &= ~(MACHNET_MSGBUF_FLAGS_SG);

  // We have finished copying over the message. Now we need to update the
  // message metadata.
  // Mark the first buffer of the message as the head of the message, and also
  // piggyback any flags requested by the application (e.g., delivery
  // notification).
  MachnetMsgBuf_t first = buffers[0];
  first.flags |= MACHNET_MSGBUF_FLAGS_SYN;
  first.flags |= (msghdr->flags & MACHNET_MSGBUF_NOTIFY_DELIVERY);
  first.flow = msghdr->flow_info;
  first.msg_len = msghdr->msg_size;
  //  first.last = buffer_indices[buffers_nr - 1];  // Link to the last buffer.

  // Finally, send the message.
  // TODO(ilias): Add retries if the ring is full, and add statistics.
  // TODO(vjabrayilov): Add logging and back off here
  if (__machnet_channel_app_ring_enqueue(ctx, buffers_nr, buffers) !=
      buffers_nr) {
    fprintf(stderr, "Cannot enqueue\n");
    free(buffers);
    return -1;
  }
  //  fprintf(stderr, "Enqueued %d buffers\n", buffers_nr);
  free(buffers);
  //  fprintf(stderr, "App ring contains %d buffers\n",
  //          __machnet_channel_app_ring_pending(ctx));
  return 0;
}

int machnet_sendmmsg(const void *channel_ctx,
                     const MachnetMsgHdr_t *msghdr_iovec, int vlen) {
  int msg_sent = 0;

  for (int msg_index = 0; msg_index < vlen; msg_index++) {
    const MachnetMsgHdr_t *msghdr = &msghdr_iovec[msg_index];
    assert(msghdr->msg_iov != NULL);
    if (machnet_sendmsg(channel_ctx, msghdr) != 0) return msg_sent;
    msg_sent++;
  }

  return msg_sent;
}

ssize_t machnet_recv(const void *channel_ctx, void *buf, size_t len,
                     MachnetFlow_t *flow) {
  MachnetMsgHdr_t msghdr;
  MachnetIovec_t iov;
  iov.base = buf;
  iov.len = len;
  msghdr.msg_iov = &iov;
  msghdr.msg_iovlen = 1;

  const int ret = machnet_recvmsg(channel_ctx, &msghdr);
  if (ret <= 0) return ret;  // No message available, or error code

  *flow = msghdr.flow_info;
  return msghdr.msg_size;
}
int machnet_recvmsg_buf(const void *channel_ctx, MachnetMsgBuf_t *msgbuf) {
  assert(channel_ctx != NULL);
  const MachnetChannelCtx_t *ctx = (const MachnetChannelCtx_t *)channel_ctx;
  uint32_t n = __machnet_channel_machnet_ring_dequeue(ctx, 1, msgbuf);
  if (n != 1) return 0;
  return 1;
}
int machnet_recvmsg(const void *channel_ctx, MachnetMsgHdr_t *msghdr) {
  assert(channel_ctx != NULL);
  assert(msghdr != NULL);
  const MachnetChannelCtx_t *ctx = (const MachnetChannelCtx_t *)channel_ctx;
  MachnetMsgBuf_t buffer;
  uint32_t n = __machnet_channel_machnet_ring_dequeue(ctx, 1, &buffer);
  if (n != 1) return 0;
  //  msghdr->msg_size = MTU;
  //  memcpy(msghdr->msg_iov->base, buffer.data, MTU);
  return 1;
  //  const uint32_t kBufferBatchSize = 16;
  //  uint32_t ret __attribute__((unused));
  //
  //  // Deque a message from the ring.
  ////  MachnetRingSlot_t buffer_index;
  //  uint32_t n = __machnet_channel_machnet_ring_dequeue(ctx, 1,
  //  &buffer_index); if (n != 1) return 0;  // No message available.
  //
  //  MachnetMsgBuf_t *buffer;
  //  buffer = __machnet_channel_buf(ctx, buffer_index);
  //  MachnetFlow_t flow_info = buffer->flow;
  //  uint32_t buf_data_ofs = 0;
  //  size_t iov_index = 0;
  //  uint32_t seg_data_ofs = 0;
  //  uint32_t total_bytes_copied = 0;
  //
  //  // `buffer_indices' array is being used to track used buffers, for later
  //  // release.
  //  MachnetRingSlot_t buffer_indices[kBufferBatchSize];
  //  uint32_t buffer_indices_index = 0;
  //
  //  while (buffer != NULL &&
  //         __machnet_channel_buf_data_len(buffer) > buf_data_ofs) {
  //    if (unlikely(iov_index >= msghdr->msg_iovlen)) {
  //      // We have already used all the segments provided, but there are more
  //      // data in this message.
  //      goto fail;
  //    }
  //
  //    // Get the source buffer.
  //    uchar_t *buf_data = __machnet_channel_buf_data_ofs(buffer,
  //    buf_data_ofs);
  //
  //    // Get the destination segment.
  //    assert(msghdr->msg_iov != NULL);
  //    const size_t seg_len = msghdr->msg_iov[iov_index].len;
  //    if (unlikely(seg_len == 0)) {
  //      // At the unlikely event of a zero-sized segment, move to the next.
  //      iov_index++;
  //      continue;
  //    }
  //
  //    assert(msghdr->msg_iov[iov_index].base != NULL);
  //    uchar_t *seg_data =
  //        (uchar_t *)msghdr->msg_iov[iov_index].base + seg_data_ofs;
  //
  //    // Copy the appropriate amount of data over.
  //    uint32_t remaining_bytes_in_buf =
  //        __machnet_channel_buf_data_len(buffer) - buf_data_ofs;
  //    uint32_t remaining_space_in_seg = seg_len - seg_data_ofs;
  //    uint32_t nbytes_to_copy =
  //        MIN(remaining_space_in_seg, remaining_bytes_in_buf);
  //    memcpy(seg_data, buf_data, nbytes_to_copy);
  //    buf_data_ofs += nbytes_to_copy;
  //    seg_data_ofs += nbytes_to_copy;
  //    total_bytes_copied += nbytes_to_copy;
  //
  //    // Have we copied the entire buffer?
  //    if (buf_data_ofs == __machnet_channel_buf_data_len(buffer)) {
  //      // Mark the buffer for later release.
  //      buffer_indices[buffer_indices_index++] = buffer_index;
  //
  //      // Get the next buffer index, if any.
  //      if (buffer->flags & MACHNET_MSGBUF_FLAGS_SG) {
  //        // This is the last buffer of the message.
  //        buffer_index = buffer->next;
  //        buffer = __machnet_channel_buf(ctx, buffer_index);
  //        buf_data_ofs = 0;
  //      }
  //
  //      // Do a batch buffer release if we reached the threshold.
  //      if (buffer_indices_index == kBufferBatchSize) {
  //        ret = __machnet_channel_buf_free_bulk(ctx, buffer_indices_index,
  //                                              buffer_indices);
  //        assert(ret == buffer_indices_index);
  //        buffer_indices_index = 0;
  //      }
  //    }
  //
  //    // Grab the next segment, if no space in this one.
  //    if (seg_data_ofs == seg_len) {
  //      iov_index++;
  //      seg_data_ofs = 0;
  //    }
  //  }
  //
  //  // We have finished copying over the message. Now add the control data.
  //  msghdr->msg_size = total_bytes_copied;
  //  msghdr->flow_info = flow_info;
  //
  //  // Free up any remaining buffers.
  //  ret = __machnet_channel_buf_free_bulk(ctx, buffer_indices_index,
  //                                        buffer_indices);
  //  assert(ret == buffer_indices_index);
  //
  //  // Success.
  //  return 1;
  //
  // fail:
  //  while (buffer != NULL) {
  //    buffer_indices[buffer_indices_index++] = buffer_index;
  //    if (buffer->flags & MACHNET_MSGBUF_FLAGS_SG) {
  //      buffer_index = buffer->next;
  //      buffer = __machnet_channel_buf(ctx, buffer_index);
  //    } else {
  //      buffer = NULL;
  //    }
  //    if (buffer == NULL || buffer_indices_index == kBufferBatchSize) {
  //      ret = __machnet_channel_buf_free_bulk(ctx, buffer_indices_index,
  //                                            buffer_indices);
  //      assert(ret == buffer_indices_index);
  //      buffer_indices_index = 0;
  //    }
  //  }
  //
  //  return -1;
}

void machnet_detach(const MachnetChannelCtx_t *ctx) {}
