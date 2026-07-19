# Spinel bundled `socket` — a carried-C spin package (Path B typed object).
#
# The socket module wraps BSD sockets. The OS-touching primitives live in
# packages/socket/sp_socket.c (linked only when `require "socket"` appears).
# The declarations below are the compiler's entire knowledge of the native
# classes: it registers each as a native class (poly-capable, GC-managed,
# cls_id-dispatched) and emits direct typed calls to the declared C symbols.
# Constructors receive the assigned cls_id first.
#
#   native_lib "name"            -- require-gate feature name
#   native_obj "packages/.../x.o" -- carried C, linked on demand
#   native_struct "Name", "sp_X", ["free"]  -- a C-backed class
#   native_new    [arg_specs], "csym"        -- arity-keyed constructors (receive cls_id)
#   native_method :name, [arg_specs], ret, "csym"  -- instance method on the class
#   native_func   :name, [arg_specs], ret, "csym"  -- module/class-level function
#     specs: any | string | string? | int | float | bool | nil | self
#
# IMPORTANT restrictions discovered while building this package:
#   * native methods are NOT inherited through the superclass chain — a native
#     method declared on BasicSocket is NOT visible on a TCPSocket instance. So
#     every concrete socket class declares the methods it needs (they share the
#     same underlying sp_Socket struct and just read fd). See SOCKET_IMPL.md §4.
#   * A user-defined instance method on a native class CANNOT call a native
#     method on `self` (the regular method lookup does not consult the
#     native_method table). So any operation that needs native-on-self is itself
#     a native_method (e.g. BasicSocket#close, #recvfrom, #local_address). The
#     pure-Ruby sugar that does NOT touch self's native methods lives in the
#     user classes (Socket.tcp, Addrinfo connect/bind sugar, ...).
#   * A native_struct class cannot host native_func lookups from inside its own
#     method bodies, so the class-level Socket.* helpers live in the plain
#     SocketN module and Socket delegates to it.
#   * Addrinfo is implemented as a PURE-RUBY class (no native_struct): it stores
#     the binary sockaddr + family/socktype/proto as ivars and uses the SocketN
#     module functions to parse them. This avoids the native-on-self problem
#     entirely while keeping the same public API.
module SocketPackage
  native_lib "socket"

  # ---- BasicSocket (shared prefix of every socket class) ----
  native_struct "BasicSocket", "sp_Socket", "sp_BasicSocket_free"
  native_method :close_read,   [],              :int,    "sp_BasicSocket_close_read"
  native_method :close_write,  [],              :int,    "sp_BasicSocket_close_write"
  native_method :shutdown,     [:int],          :int,    "sp_BasicSocket_shutdown"
  native_method :setsockopt,   [:int, :int, :string], :int, "sp_BasicSocket_setsockopt"
  native_method :getsockname,  [],              :string, "sp_BasicSocket_getsockname"
  native_method :getpeername,  [],              :string, "sp_BasicSocket_getpeername"
  native_method :recv,         [:int],          :string, "sp_BasicSocket_recv"
  native_method :bind,      [:string], :int, "sp_Socket_bind_raw"
  native_method :connect,   [:string], :int, "sp_Socket_connect_raw"
  native_method :sendto,    [:string, :int, :string], :int, "sp_Socket_sendto_raw"
  native_method :listen,    [:int],    :int, "sp_Socket_listen_raw"
  native_method :send,         [:string],       :int,    "sp_BasicSocket_send"
  native_method :fileno,       [],              :int,    "sp_BasicSocket_fileno"
  native_method :closed?,      [],              :bool,   "sp_BasicSocket_closed_p"
  native_method :to_i,         [],              :int,    "sp_BasicSocket_fileno"
  # self-touching ops are themselves native (see module-restriction note above)
  native_method :close,          [],        :int, "sp_BasicSocket_close"
  native_method :recvfrom_raw,    [:int],     :any, "sp_BasicSocket_recvfrom_raw"
  native_method :sendmsg,        [:string],  :int, "sp_BasicSocket_sendmsg"
  native_method :recvmsg_raw,     [:int],     :any, "sp_BasicSocket_recvmsg_raw"
  native_method :read_nonblock,  [:int],     :string, "sp_BasicSocket_read_nonblock"
  native_method :write_nonblock, [:string],  :int, "sp_BasicSocket_write_nonblock"

  # ---- Socket ----
  native_struct "Socket", "sp_Socket", "sp_BasicSocket_free"
  native_new [:int, :int, :int], "sp_Socket_new"          # Socket.new(family, type, proto)
  native_new [:int],            "sp_Socket_from_fd_wrapper" # Socket.new(fd)
  native_method :connect,   [:string], :int, "sp_Socket_connect_raw"
  native_method :bind,      [:string], :int, "sp_Socket_bind_raw"
  native_method :listen,    [:int],    :int, "sp_Socket_listen_raw"
  native_method :accept,    [],       :int, "sp_Socket_accept_raw"
  native_method :send,      [:string], :int, "sp_Socket_send_raw"
  native_method :sendto,    [:string, :int, :string], :int, "sp_Socket_sendto_raw"
  native_method :close_read,   [],              :int,    "sp_BasicSocket_close_read"
  native_method :close_write,  [],              :int,    "sp_BasicSocket_close_write"
  native_method :shutdown,     [:int],          :int,    "sp_BasicSocket_shutdown"
  native_method :setsockopt,   [:int, :int, :string], :int, "sp_BasicSocket_setsockopt"
  native_method :getsockname,  [],              :string, "sp_BasicSocket_getsockname"
  native_method :getpeername,  [],              :string, "sp_BasicSocket_getpeername"
  native_method :recv,         [:int],          :string, "sp_BasicSocket_recv"
  native_method :bind,      [:string], :int, "sp_Socket_bind_raw"
  native_method :connect,   [:string], :int, "sp_Socket_connect_raw"
  native_method :sendto,    [:string, :int, :string], :int, "sp_Socket_sendto_raw"
  native_method :listen,    [:int],    :int, "sp_Socket_listen_raw"
  native_method :fileno,       [],              :int,    "sp_BasicSocket_fileno"
  native_method :closed?,      [],              :bool,   "sp_BasicSocket_closed_p"
  native_method :to_i,         [],              :int,    "sp_BasicSocket_fileno"
  native_method :close,          [],        :int, "sp_BasicSocket_close"
  native_method :recvfrom_raw,    [:int],     :any, "sp_BasicSocket_recvfrom_raw"
  native_method :sendmsg,        [:string],  :int, "sp_BasicSocket_sendmsg"
  native_method :recvmsg_raw,     [:int],     :any, "sp_BasicSocket_recvmsg_raw"
  native_method :read_nonblock,  [:int],     :string, "sp_BasicSocket_read_nonblock"
  native_method :write_nonblock, [:string],  :int, "sp_BasicSocket_write_nonblock"

  # ---- IPSocket ----
  native_struct "IPSocket", "sp_Socket", "sp_BasicSocket_free"
  native_method :close_read,   [],              :int,    "sp_BasicSocket_close_read"
  native_method :close_write,  [],              :int,    "sp_BasicSocket_close_write"
  native_method :shutdown,     [:int],          :int,    "sp_BasicSocket_shutdown"
  native_method :setsockopt,   [:int, :int, :string], :int, "sp_BasicSocket_setsockopt"
  native_method :getsockname,  [],              :string, "sp_BasicSocket_getsockname"
  native_method :getpeername,  [],              :string, "sp_BasicSocket_getpeername"
  native_method :recv,         [:int],          :string, "sp_BasicSocket_recv"
  native_method :bind,      [:string], :int, "sp_Socket_bind_raw"
  native_method :connect,   [:string], :int, "sp_Socket_connect_raw"
  native_method :sendto,    [:string, :int, :string], :int, "sp_Socket_sendto_raw"
  native_method :listen,    [:int],    :int, "sp_Socket_listen_raw"
  native_method :send,         [:string],       :int,    "sp_BasicSocket_send"
  native_method :fileno,       [],              :int,    "sp_BasicSocket_fileno"
  native_method :closed?,      [],              :bool,   "sp_BasicSocket_closed_p"
  native_method :to_i,         [],              :int,    "sp_BasicSocket_fileno"
  native_method :close,          [],        :int, "sp_BasicSocket_close"
  native_method :recvfrom_raw,    [:int],     :any, "sp_BasicSocket_recvfrom_raw"
  native_method :sendmsg,        [:string],  :int, "sp_BasicSocket_sendmsg"
  native_method :recvmsg_raw,     [:int],     :any, "sp_BasicSocket_recvmsg_raw"
  native_method :read_nonblock,  [:int],     :string, "sp_BasicSocket_read_nonblock"
  native_method :write_nonblock, [:string],  :int, "sp_BasicSocket_write_nonblock"

  # ---- TCPSocket ----
  native_struct "TCPSocket", "sp_Socket", "sp_BasicSocket_free"
  native_new [:string, :int], "sp_TCPSocket_new"          # TCPSocket.new(host, port)
  native_new [:int],          "sp_Socket_from_fd_wrapper" # TCPSocket.new(fd)
  native_method :close_read,   [],              :int,    "sp_BasicSocket_close_read"
  native_method :close_write,  [],              :int,    "sp_BasicSocket_close_write"
  native_method :shutdown,     [:int],          :int,    "sp_BasicSocket_shutdown"
  native_method :setsockopt,   [:int, :int, :string], :int, "sp_BasicSocket_setsockopt"
  native_method :getsockname,  [],              :string, "sp_BasicSocket_getsockname"
  native_method :getpeername,  [],              :string, "sp_BasicSocket_getpeername"
  native_method :recv,         [:int],          :string, "sp_BasicSocket_recv"
  native_method :bind,      [:string], :int, "sp_Socket_bind_raw"
  native_method :connect,   [:string], :int, "sp_Socket_connect_raw"
  native_method :sendto,    [:string, :int, :string], :int, "sp_Socket_sendto_raw"
  native_method :listen,    [:int],    :int, "sp_Socket_listen_raw"
  native_method :send,         [:string],       :int,    "sp_BasicSocket_send"
  native_method :fileno,       [],              :int,    "sp_BasicSocket_fileno"
  native_method :closed?,      [],              :bool,   "sp_BasicSocket_closed_p"
  native_method :to_i,         [],              :int,    "sp_BasicSocket_fileno"
  native_method :close,          [],        :int, "sp_BasicSocket_close"
  native_method :recvfrom_raw,    [:int],     :any, "sp_BasicSocket_recvfrom_raw"
  native_method :sendmsg,        [:string],  :int, "sp_BasicSocket_sendmsg"
  native_method :recvmsg_raw,     [:int],     :any, "sp_BasicSocket_recvmsg_raw"
  native_method :read_nonblock,  [:int],     :string, "sp_BasicSocket_read_nonblock"
  native_method :write_nonblock, [:string],  :int, "sp_BasicSocket_write_nonblock"

  # ---- TCPServer ----
  native_struct "TCPServer", "sp_Socket", "sp_BasicSocket_free"
  native_new [:string, :int], "sp_TCPServer_new"   # TCPServer.new(host, port)
  native_new [:int],          "sp_TCPServer_new_port" # TCPServer.new(port)
  # accept is implemented as a Ruby method (TCPServer#accept) that wraps the
  # accepted fd in a TCPSocket; it must NOT also be a native_method returning an
  # int, or dispatch would pick the native int-returning form and `.sendmsg` on
  # the resulting Integer would fail.
  native_method :close_read,   [],              :int,    "sp_BasicSocket_close_read"
  native_method :close_write,  [],              :int,    "sp_BasicSocket_close_write"
  native_method :shutdown,     [:int],          :int,    "sp_BasicSocket_shutdown"
  native_method :setsockopt,   [:int, :int, :string], :int, "sp_BasicSocket_setsockopt"
  native_method :getsockname,  [],              :string, "sp_BasicSocket_getsockname"
  native_method :getpeername,  [],              :string, "sp_BasicSocket_getpeername"
  native_method :recv,         [:int],          :string, "sp_BasicSocket_recv"
  native_method :bind,      [:string], :int, "sp_Socket_bind_raw"
  native_method :connect,   [:string], :int, "sp_Socket_connect_raw"
  native_method :sendto,    [:string, :int, :string], :int, "sp_Socket_sendto_raw"
  native_method :listen,    [:int],    :int, "sp_Socket_listen_raw"
  native_method :send,         [:string],       :int,    "sp_BasicSocket_send"
  native_method :fileno,       [],              :int,    "sp_BasicSocket_fileno"
  native_method :closed?,      [],              :bool,   "sp_BasicSocket_closed_p"
  native_method :to_i,         [],              :int,    "sp_BasicSocket_fileno"
  native_method :close,          [],        :int, "sp_BasicSocket_close"
  native_method :recvfrom_raw,    [:int],     :any, "sp_BasicSocket_recvfrom_raw"
  native_method :sendmsg,        [:string],  :int, "sp_BasicSocket_sendmsg"
  native_method :recvmsg_raw,     [:int],     :any, "sp_BasicSocket_recvmsg_raw"
  native_method :read_nonblock,  [:int],     :string, "sp_BasicSocket_read_nonblock"
  native_method :write_nonblock, [:string],  :int, "sp_BasicSocket_write_nonblock"

  # ---- UDPSocket ----
  native_struct "UDPSocket", "sp_Socket", "sp_BasicSocket_free"
  native_new [:int], "sp_UDPSocket_new"   # UDPSocket.new(family)
  native_method :close_read,   [],              :int,    "sp_BasicSocket_close_read"
  native_method :close_write,  [],              :int,    "sp_BasicSocket_close_write"
  native_method :shutdown,     [:int],          :int,    "sp_BasicSocket_shutdown"
  native_method :setsockopt,   [:int, :int, :string], :int, "sp_BasicSocket_setsockopt"
  native_method :getsockname,  [],              :string, "sp_BasicSocket_getsockname"
  native_method :getpeername,  [],              :string, "sp_BasicSocket_getpeername"
  native_method :recv,         [:int],          :string, "sp_BasicSocket_recv"
  native_method :bind,      [:string], :int, "sp_Socket_bind_raw"
  native_method :connect,   [:string], :int, "sp_Socket_connect_raw"
  native_method :sendto,    [:string, :int, :string], :int, "sp_Socket_sendto_raw"
  native_method :listen,    [:int],    :int, "sp_Socket_listen_raw"
  native_method :send,         [:string],       :int,    "sp_BasicSocket_send"
  native_method :fileno,       [],              :int,    "sp_BasicSocket_fileno"
  native_method :closed?,      [],              :bool,   "sp_BasicSocket_closed_p"
  native_method :to_i,         [],              :int,    "sp_BasicSocket_fileno"
  native_method :close,          [],        :int, "sp_BasicSocket_close"
  native_method :recvfrom_raw,    [:int],     :any, "sp_BasicSocket_recvfrom_raw"
  native_method :sendmsg,        [:string],  :int, "sp_BasicSocket_sendmsg"
  native_method :recvmsg_raw,     [:int],     :any, "sp_BasicSocket_recvmsg_raw"
  native_method :read_nonblock,  [:int],     :string, "sp_BasicSocket_read_nonblock"
  native_method :write_nonblock, [:string],  :int, "sp_BasicSocket_write_nonblock"

  # ---- UNIXSocket ----
  native_struct "UNIXSocket", "sp_Socket", "sp_BasicSocket_free"
  native_new [:string], "sp_UNIXSocket_new"   # UNIXSocket.new(path)
  native_new [:int],    "sp_Socket_from_fd_wrapper" # UNIXSocket.new(fd)
  native_method :close_read,   [],              :int,    "sp_BasicSocket_close_read"
  native_method :close_write,  [],              :int,    "sp_BasicSocket_close_write"
  native_method :shutdown,     [:int],          :int,    "sp_BasicSocket_shutdown"
  native_method :setsockopt,   [:int, :int, :string], :int, "sp_BasicSocket_setsockopt"
  native_method :getsockname,  [],              :string, "sp_BasicSocket_getsockname"
  native_method :getpeername,  [],              :string, "sp_BasicSocket_getpeername"
  native_method :recv,         [:int],          :string, "sp_BasicSocket_recv"
  native_method :bind,      [:string], :int, "sp_Socket_bind_raw"
  native_method :connect,   [:string], :int, "sp_Socket_connect_raw"
  native_method :sendto,    [:string, :int, :string], :int, "sp_Socket_sendto_raw"
  native_method :listen,    [:int],    :int, "sp_Socket_listen_raw"
  native_method :send,         [:string],       :int,    "sp_BasicSocket_send"
  native_method :fileno,       [],              :int,    "sp_BasicSocket_fileno"
  native_method :closed?,      [],              :bool,   "sp_BasicSocket_closed_p"
  native_method :to_i,         [],              :int,    "sp_BasicSocket_fileno"
  native_method :close,          [],        :int, "sp_BasicSocket_close"
  native_method :recvfrom_raw,    [:int],     :any, "sp_BasicSocket_recvfrom_raw"
  native_method :sendmsg,        [:string],  :int, "sp_BasicSocket_sendmsg"
  native_method :recvmsg_raw,     [:int],     :any, "sp_BasicSocket_recvmsg_raw"
  native_method :read_nonblock,  [:int],     :string, "sp_BasicSocket_read_nonblock"
  native_method :write_nonblock, [:string],  :int, "sp_BasicSocket_write_nonblock"

  # ---- UNIXServer ----
  native_struct "UNIXServer", "sp_Socket", "sp_BasicSocket_free"
  native_new [:string], "sp_UNIXServer_new"   # UNIXServer.new(path)
  native_method :accept,    [],       :int, "sp_Socket_accept_raw"
  native_method :close_read,   [],              :int,    "sp_BasicSocket_close_read"
  native_method :close_write,  [],              :int,    "sp_BasicSocket_close_write"
  native_method :shutdown,     [:int],          :int,    "sp_BasicSocket_shutdown"
  native_method :setsockopt,   [:int, :int, :string], :int, "sp_BasicSocket_setsockopt"
  native_method :getsockname,  [],              :string, "sp_BasicSocket_getsockname"
  native_method :getpeername,  [],              :string, "sp_BasicSocket_getpeername"
  native_method :recv,         [:int],          :string, "sp_BasicSocket_recv"
  native_method :bind,      [:string], :int, "sp_Socket_bind_raw"
  native_method :connect,   [:string], :int, "sp_Socket_connect_raw"
  native_method :sendto,    [:string, :int, :string], :int, "sp_Socket_sendto_raw"
  native_method :listen,    [:int],    :int, "sp_Socket_listen_raw"
  native_method :send,         [:string],       :int,    "sp_BasicSocket_send"
  native_method :fileno,       [],              :int,    "sp_BasicSocket_fileno"
  native_method :closed?,      [],              :bool,   "sp_BasicSocket_closed_p"
  native_method :to_i,         [],              :int,    "sp_BasicSocket_fileno"
  native_method :close,          [],        :int, "sp_BasicSocket_close"
  native_method :recvfrom_raw,    [:int],     :any, "sp_BasicSocket_recvfrom_raw"
  native_method :sendmsg,        [:string],  :int, "sp_BasicSocket_sendmsg"
  native_method :recvmsg_raw,     [:int],     :any, "sp_BasicSocket_recvmsg_raw"
  native_method :read_nonblock,  [:int],     :string, "sp_BasicSocket_read_nonblock"
  native_method :write_nonblock, [:string],  :int, "sp_BasicSocket_write_nonblock"
end

# Plain module carrying the class-level (module) native functions. A plain
# module (not a native_struct class) resolves native_func on its constant from
# anywhere, including from inside Socket's own methods.
module SocketN
  native_lib "socket"
  native_obj "packages/socket/sp_socket.o"
  native_func :gethostname,  [],               :string, "sp_Socket_gethostname"
  native_func :getservbyname,[:string],         :string, "sp_Socket_getservbyname"
  native_func :getservbyport,[:int],           :int,    "sp_Socket_getservbyport"
  native_func :sockaddr_in,  [:string, :int],  :string, "sp_Socket_pack_in_wrap"
  native_func :pack_sockaddr_in, [:string, :int], :string, "sp_Socket_pack_in_wrap"
  native_func :unpack_sockaddr_in, [:string],  :string, "sp_Socket_in_addr_wrap"
  native_func :sockaddr_un,  [:string],        :string, "sp_Socket_pack_un_wrap"
  native_func :pack_sockaddr_un, [:string],    :string, "sp_Socket_pack_un_wrap"
  native_func :unpack_sockaddr_un, [:string],  :string, "sp_Socket_un_path_wrap"
  native_func :family_of,         [:string],  :int,    "sp_Socket_family_wrap"
  native_func :getaddrinfo,  [:string, :string, :int, :int, :int, :int], :any, "sp_Socket_getaddrinfo_strings"
  native_func :socketpair,   [:int, :int, :int], :any, "sp_Socket_socketpair_fds"
  native_func :accept_raw,   [:any], :int, "sp_Socket_accept_one_raw"
  native_func :unpack_sockaddr_in_port, [:string], :int, "sp_Socket_in_port_wrap"
end

# ===========================================================================
# User-facing classes. The < IO / < BasicSocket superclass edges are dropped
# (parent stays -1) because IO/File is a builtin, not a Spinel user class.
# Shared behavior is reimplemented in Ruby below. Addrinfo is a PURE-RUBY
# class (see the note at the top of this file).
# ===========================================================================

class BasicSocket
  def sendmsg_nonblock(mesg, flags = 0, dest_sockaddr = nil, *controls, exception: true); self.sendmsg(mesg); end
  def recvmsg_nonblock(dlen = nil, flags = 0, clen = nil, scm_rights: false); self.recvmsg(dlen, flags); end
  def connect_address; Addrinfo.new(self.getsockname); end
  # sendmsg/recvmsg/local_address/remote_address are themselves native_methods
  # (see the native_method table above); a user wrapper would clash with the
  # native forward-decl symbol, so no Ruby wrapper is provided here.

  # recvfrom/recvmsg return [mesg, binary_sockaddr] from the native layer; we
  # wrap the binary sockaddr in a pure-Ruby Addrinfo so accessors work.
  # NOTE: recvfrom/recvmsg are implemented as Ruby wrappers around the native
  # recvfrom_raw/recvmsg_raw (which return [mesg, binary_sockaddr]). A native
  # method and a Ruby method cannot share a name, so the native symbols are
  # suffixed with _raw. The compiler also does not propagate the caller's
  # (maxlen, flags) arity into the native call for these (it synthesizes a 2-arg
  # forward-decl that conflicts with the header), so we call the 1-arg raw form
  # and ignore the Ruby-level flags (MSG_* flags are out of scope for this pass).
  def recvfrom(maxlen, flags = 0)
    mesg, bin = self.recvfrom_raw(maxlen)
    [mesg, Addrinfo.new(bin)]
  end
  def recvmsg(dlen = nil, flags = 0)
    mesg, bin = self.recvmsg_raw(dlen || 65536)
    [mesg, Addrinfo.new(bin)]
  end

  def self.do_not_reverse_lookup; false; end
  def self.do_not_reverse_lookup=(v); nil; end
  def do_not_reverse_lookup; false; end
  def do_not_reverse_lookup=(v); nil; end
  attr_reader :do_not_reverse_lookup

  def for_fd(fd); Socket.for_fd(fd); end

  # local_address / remote_address wrap the native getsockname / getpeername
  # (binary sockaddr strings) in a real (pure-Ruby) Addrinfo, whose @sockaddr
  # ivar the Addrinfo accessors read. They cannot be native_methods returning a
  # C sp_Addrinfo, because that object lacks the @sockaddr ivar the Ruby
  # Addrinfo class expects.
  def local_address; Addrinfo.new(self.getsockname); end
  def remote_address; Addrinfo.new(self.getpeername); end
end

class Socket < BasicSocket
  def connect(sockaddr); self.connect(sockaddr); 0; end
  def bind(sockaddr); self.bind(sockaddr); 0; end
  def listen(backlog); self.listen(backlog); 0; end
  def accept; Socket.new(SocketN.accept_raw(self)); end
  def sysaccept; self.accept; end

  def connect_nonblock(addr, exception: true); self.connect(addr); 0; end
  def accept_nonblock(exception: true); self.accept; end
  def recvfrom_nonblock(len, flag = 0, outbuf = nil, exception: true); self.recvfrom(len, flag); end

  def self.getnameinfo(*args); nil; end
  def self.gethostname; SocketN.gethostname; end
  def self.getservbyname(name, proto = "tcp"); SocketN.getservbyname(name); end
  def self.getservbyport(port, proto = "tcp"); SocketN.getservbyport(port); end
  def self.sockaddr_in(port, host); SocketN.sockaddr_in(host.to_s, port); end
  def self.pack_sockaddr_in(port, host); SocketN.sockaddr_in(host.to_s, port); end
  def self.unpack_sockaddr_in(sockaddr)
    [SocketN.unpack_sockaddr_in(sockaddr), SocketN.unpack_sockaddr_in_port(sockaddr)]
  end
  def self.unpack_sockaddr_in_port(sockaddr); SocketN.unpack_sockaddr_in_port(sockaddr); end
  def self.sockaddr_un(path); SocketN.sockaddr_un(path.to_s); end
  def self.pack_sockaddr_un(path); SocketN.sockaddr_un(path.to_s); end
  def self.unpack_sockaddr_un(sockaddr); SocketN.unpack_sockaddr_un(sockaddr); end

  def self.getaddrinfo(host, service, family = nil, socktype = nil, protocol = nil, flags = nil)
    svc = (service && !service.is_a?(String) ? service.to_s : service)
    # socktype/protocol may arrive as CRuby symbols (:STREAM/:DGRAM/:RAW);
    # map them to their integer SOCK_* values before the native call.
    st = case socktype
         when :STREAM then Socket::SOCK_STREAM
         when :DGRAM  then Socket::SOCK_DGRAM
         when :RAW    then Socket::SOCK_RAW
         else (socktype || 0).to_i
         end
    pt = case protocol
         when :TCP then Socket::IPPROTO_TCP
         when :UDP then Socket::IPPROTO_UDP
         else (protocol || 0).to_i
         end
    SocketN.getaddrinfo(
      host.to_s,
      svc,
      (family || 0).to_i,
      st,
      pt,
      (flags || 0).to_i)
  end
  def self.socketpair(domain, type, protocol = 0)
    SocketN.socketpair(domain, type, protocol)
  end
  def self.pair(*a); Socket.socketpair(*a); end
  def self.ip_address_list; []; end
  def self.tcp_fast_fallback; true; end
  def self.tcp_fast_fallback=(v); nil; end
  attr_accessor :tcp_fast_fallback

  def ipv6only!; nil; end
end

class IPSocket < BasicSocket
  def addr
    a = self.local_address
    [a.ip_address, a.ip_port]
  end
  def peeraddr; [self.remote_address.to_sockaddr]; end
  def self.getaddress(host); Addrinfo.getaddrinfo(host, "http").first.ip_address; end
end

class TCPSocket < IPSocket
  def self.open(*a, &b); s = TCPSocket.new(*a); b ? (yield s; s.close; nil) : s; end
end

class TCPServer < TCPSocket
  def accept
    fd = SocketN.accept_raw(self)
    TCPSocket.new(fd)
  end
  def accept_nonblock(exception: true); self.accept; end
end

class UDPSocket < IPSocket
  # NOTE: UDPSocket#connect(host, port) / #bind(host, port) / #send(mesg, flags,
  # host, port) sugar is intentionally omitted. The compiler (this first pass)
  # routes a 1-arg self.bind(sockaddr) call into the 2-arg Ruby method (filling
  # the missing port with a default), which would re-resolve the host as a
  # binary sockaddr and fail in getaddrinfo. Callers pass a sockaddr string to
  # the inherited BasicSocket#bind/#connect (e.g. udp.bind(Socket.sockaddr_in(port, host)))
  # or use UDPSocket#connect/#bind with an explicit Addrinfo, matching CRuby's
  # underlying API.
  def send(mesg, flags = 0, *rest)
    if rest.length >= 2
      host, port = rest[0], rest[1]
      ai = Addrinfo.getaddrinfo(host.to_s, port.to_i, Socket::AF_INET, :DGRAM)[0]
      self.sendto(mesg, flags, ai.to_sockaddr)
    else
      self.send(mesg)
    end
  end
  def recvfrom_nonblock(len, flag = 0, outbuf = nil, exception: true); self.recvfrom(len, flag); end
  def getsockname_family; Socket::AF_INET; end
end

class UNIXSocket < BasicSocket
  def path; self.local_address.unix_path; end
  def addr
    a = self.local_address
    [a.ip_address, a.ip_port]
  end
  def peeraddr; [self.remote_address.to_sockaddr]; end
  def recvfrom_nonblock(len, flags = 0); self.recvfrom(len, flags); end
  def send_io(*a); nil; end
  def recv_io(*a); nil; end
  def self.socketpair(type = :STREAM)
    fds = Socket.socketpair(Socket::AF_UNIX, Socket::SOCK_STREAM, 0)
    [UNIXSocket.new(fds[0]), UNIXSocket.new(fds[1])]
  end
  def self.pair(*a); UNIXSocket.socketpair(*a); end
end

class UNIXServer < UNIXSocket
  def accept
    fd = SocketN.accept_raw(self)
    UNIXSocket.new(fd)
  end
  def accept_nonblock(exception: true); self.accept; end
end

# ---------------------------------------------------------------------------
# Addrinfo — pure-Ruby class. Stores the binary sockaddr plus the
# address family / socktype / protocol as ivars; uses the SocketN module
# functions to parse them. Mirrors the public API of CRuby's Addrinfo.
# ---------------------------------------------------------------------------
class Addrinfo
  # Canonical constructor: Addrinfo.new(family, socktype, protocol, binary_sockaddr)
  # and Addrinfo.new(binary_sockaddr). The (host, port) array form is built by
  # the .new_from_array / .tcp / .ip / .udp / .unix class helpers below.
  def initialize(family, socktype = nil, protocol = nil, sockaddr = nil)
    if sockaddr.nil? && family.is_a?(String) && family.bytes.length >= 2
      # Addrinfo.new(binary_sockaddr): the single-arg form. Derive the family
      # from the binary sockaddr's sa_family.
      sockaddr = family
      family = SocketN.family_of(sockaddr)
    end
    @family = family
    @socktype = socktype || 0
    @protocol = protocol || 0
    @sockaddr = sockaddr
  end

  def self.new_from_array(af, port, host, family = nil, socktype = nil, protocol = nil)
    if af == "AF_INET" || af == :INET || af == Socket::AF_INET
      bin = Socket.sockaddr_in(port, host)
      fam = Socket::AF_INET
    elsif af == "AF_UNIX" || af == :UNIX || af == Socket::AF_UNIX
      bin = Socket.sockaddr_un(host)
      fam = Socket::AF_UNIX
    else
      raise SocketError, "unexpected address family"
    end
    Addrinfo.new(fam, socktype || Socket::SOCK_STREAM, protocol || 0, bin)
  end

  def ip_address; SocketN.unpack_sockaddr_in(@sockaddr); end
  def ip_port; SocketN.unpack_sockaddr_in_port(@sockaddr); end
  def unix_path; SocketN.unpack_sockaddr_un(@sockaddr); end
  def to_sockaddr; @sockaddr; end
  def afamily; @family; end
  def pfamily; @family; end
  def family; @family; end
  def socktype; @socktype; end
  def protocol; @protocol; end
  def canonname; nil; end

  def ipv4?; @family == Socket::AF_INET; end
  def ipv6?; @family == Socket::AF_INET6; end
  def unix?; @family == Socket::AF_UNIX; end
  def ip?; ipv4? || ipv6?; end

  def inspect; "#<Addrinfo: #{ip_address || unix_path}>"; end
  def inspect_sockaddr; self.inspect; end
  def to_s; @sockaddr; end
  def getnameinfo(*a); nil; end

  def self.getaddrinfo(host, service, family = nil, socktype = nil, protocol = nil, flags = nil)
    # socktype/protocol may arrive as CRuby symbols (:STREAM/:DGRAM/:RAW); map
    # them to their integer SOCK_*/IPPROTO_* values before building Addrinfo.
    st = case socktype
         when :STREAM then Socket::SOCK_STREAM
         when :DGRAM  then Socket::SOCK_DGRAM
         when :RAW    then Socket::SOCK_RAW
         else (socktype || 0).to_i
         end
    pt = case protocol
         when :TCP then Socket::IPPROTO_TCP
         when :UDP then Socket::IPPROTO_UDP
         else (protocol || 0).to_i
         end
    strs = Socket.getaddrinfo(host, service, family, socktype, protocol, flags)
    out = []
    strs.each do |bin|
      # The family comes from the binary sockaddr (so 127.0.0.1 -> AF_INET).
      # Build the canonical 4-arg Addrinfo so accessors report correctly.
      fam = family || SocketN.family_of(bin)
      ai = Addrinfo.new(fam, st, pt, bin)
      out.push(ai)
    end
    out
  end
  def self.ip(host); Addrinfo.getaddrinfo(host, nil, nil, :STREAM)[0]; end
  def self.tcp(host, port); Addrinfo.getaddrinfo(host, port, nil, :STREAM)[0]; end
  def self.udp(host, port); Addrinfo.getaddrinfo(host, port, nil, :DGRAM)[0]; end
  def self.unix(path, socktype = :STREAM)
    bin = Socket.sockaddr_un(path)
    st = (socktype == :DGRAM) ? Socket::SOCK_DGRAM : Socket::SOCK_STREAM
    Addrinfo.new(Socket::AF_UNIX, st, 0, bin)
  end

  # High-level connect/bind/listen over a freshly created Socket, mirroring
  # CRuby's Addrinfo#connect_internal.
  def connect_internal(local_addrinfo, timeout = nil)
    sock = Socket.new(self.pfamily, self.socktype, self.protocol)
    begin
      sock.bind(local_addrinfo.to_sockaddr) if local_addrinfo
      sock.connect(self.to_sockaddr)
    rescue Exception
      sock.close
      raise
    end
    if block_given?
      begin
        yield sock
      ensure
        sock.close
      end
    else
      sock
    end
  end
  protected :connect_internal

  def connect(timeout: nil, &block)
    sock = Socket.new(self.pfamily, self.socktype, self.protocol)
    begin
      sock.connect(self.to_sockaddr)
    rescue Exception
      sock.close
      raise
    end
    if block_given?
      begin
        yield sock
      ensure
        sock.close
      end
    else
      sock
    end
  end
  def connect_from(*a, timeout: nil, &b); self.connect(&b); end
  def connect_to(*a, timeout: nil, &b); self.connect(&b); end
  def bind
    sock = Socket.new(self.pfamily, self.socktype, self.protocol)
    sock.bind(self.to_sockaddr)
    sock
  end
  def listen(backlog = Socket::SOMAXCONN)
    sock = Socket.new(self.pfamily, self.socktype, self.protocol)
    sock.bind(self.to_sockaddr)
    sock.listen(backlog)
    sock
  end
  def family_addrinfo(*a); self; end
  def marshal_dump; [@sockaddr]; end
  def marshal_load(a); @sockaddr = a[0]; @family = Socket::AF_UNSPEC; @socktype = 0; @protocol = 0; end

  def ipv4_loopback?; ipv4? && ip_address == "127.0.0.1"; end
  def ipv6_loopback?; ipv6? && ip_address == "::1"; end
  def ipv4_private?; false; end
  def ipv6_unspecified?; ipv6? && ip_address == "::"; end
  def ipv6_multicast?; false; end
  def ipv6_linklocal?; false; end
  def ipv6_sitelocal?; false; end
  def ipv6_unique_local?; false; end
  def ipv6_v4mapped?; false; end
  def ipv6_v4compat?; false; end
  def ipv6_mc_nodelocal?; false; end
  def ipv6_mc_linklocal?; false; end
  def ipv6_mc_sitelocal?; false; end
  def ipv6_mc_orglocal?; false; end
  def ipv6_mc_global?; false; end
  def ipv6_to_ipv4; nil; end
end

# Socket::Option, Socket::Ifaddr, Socket::AncillaryData — carried as plain
# data classes (no C needed for the first pass).
class Socket
  class Option
    def initialize(family, level, optname, data)
      @family = family; @level = level; @optname = optname; @data = data
    end
    attr_reader :family, :level, :optname, :data
    def self.int(family, level, optname, intval)
      Socket::Option.new(family, level, optname, [intval].pack("i!"))
    end
    def self.byte(family, level, optname, byteval)
      Socket::Option.new(family, level, optname, [byteval].pack("c"))
    end
    def int; @data.unpack("i!")[0]; end
    def bool; @data.unpack("i!")[0] != 0; end
    def byte; @data.unpack("c")[0]; end
    def inspect; "#<Socket::Option #{@family} #{@level} #{@optname} #{@data.inspect}>"; end
  end

  class Ifaddr
    def initialize(name, addr = nil, netmask = nil, broadaddr = nil, dstaddr = nil)
      @name = name; @addr = addr; @netmask = netmask
      @broadaddr = broadaddr; @dstaddr = dstaddr
    end
    attr_reader :name, :addr, :netmask, :broadaddr, :dstaddr
    def inspect; "#<Socket::Ifaddr #{@name}>"; end
  end

  class AncillaryData
    def initialize(family, level, type, data)
      @family = family; @level = level; @type = type; @data = data
    end
    attr_reader :family, :level, :type, :data
    def self.int(family, level, type, intval)
      Socket::AncillaryData.new(family, level, type, [intval].pack("i!"))
    end
    def int; @data.unpack("i!")[0]; end
  end
end

# ---------------------------------------------------------------------------
# Constants (plain Ruby integer literals; values per host platform, matching
# the host CRuby build; source: ext/socket/constants.c / mkconstants.rb).
# ---------------------------------------------------------------------------
class Socket
  AF_UNSPEC     = 0
  AF_INET       = 2
  AF_INET6      = 10
  AF_UNIX       = 1
  AF_LOCAL      = 1
  PF_UNSPEC     = 0
  PF_INET       = 2
  PF_INET6      = 10
  PF_UNIX       = 1
  PF_LOCAL      = 1
  SOCK_STREAM   = 1
  SOCK_DGRAM    = 2
  SOCK_RAW      = 3
  SOCK_RDM      = 4
  SOCK_SEQPACKET = 5
  IPPROTO_IP    = 0
  IPPROTO_ICMP  = 1
  IPPROTO_TCP   = 6
  IPPROTO_UDP   = 17
  IPPROTO_RAW   = 255
  SOL_SOCKET    = 1
  SOL_IP        = 0
  SOL_TCP       = 6
  SOL_UDP       = 17
  SO_REUSEADDR  = 2
  SO_TYPE       = 3
  SO_ERROR      = 4
  SO_BROADCAST  = 6
  SO_REUSEPORT  = 15
  SO_KEEPALIVE  = 9
  SO_LINGER     = 13
  SO_OOBINLINE  = 10
  SO_SNDBUF     = 7
  SO_RCVBUF     = 8
  SO_DONTROUTE  = 5
  IP_TTL        = 4
  IP_TOS        = 1
  IP_MULTICAST_TTL = 33
  IP_MULTICAST_LOOP = 34
  IP_ADD_MEMBERSHIP = 35
  IP_DROP_MEMBERSHIP = 36
  TCP_NODELAY   = 1
  TCP_KEEPIDLE  = 4
  TCP_KEEPINTVL = 5
  TCP_KEEPCNT   = 6
  MSG_OOB       = 1
  MSG_PEEK      = 2
  MSG_DONTROUTE = 4
  MSG_EOR       = 8
  MSG_TRUNC     = 0x20
  MSG_CTRUNC    = 0x40
  MSG_WAITALL   = 0x100
  MSG_DONTWAIT  = 0x40
  SHUT_RD       = 0
  SHUT_WR       = 1
  SHUT_RDWR     = 2
  SOMAXCONN     = 128
  AI_PASSIVE    = 0x0001
  AI_CANONNAME  = 0x0002
  AI_NUMERICHOST = 0x0004
  AI_NUMERICSERV = 0x0400
  AI_V4MAPPED   = 0x0008
  AI_ALL        = 0x0010
  AI_ADDRCONFIG = 0x0020
  EAI_AGAIN     = -3
  EAI_BADFLAGS  = -1
  EAI_FAIL      = -4
  EAI_FAMILY    = -6
  EAI_MEMORY    = -10
  EAI_NONAME    = -2
  EAI_SERVICE   = -8
  EAI_SOCKTYPE  = -7
end
