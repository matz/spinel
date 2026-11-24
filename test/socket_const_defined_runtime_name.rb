require "socket"

names = %i[SOL_SOCKET SO_KEEPALIVE IPPROTO_TCP NOPE]
p names.map { |c| Socket.const_defined?(c) }
p names.map(&:to_s).map { |c| Socket.const_defined?(c) }
p %i[SOL_SOCKET SO_KEEPALIVE].all? { |c| Socket.const_defined? c }
%w[bad Not-a-name].each do |name|
  begin
    Socket.const_defined?(name)
  rescue NameError => e
    p e.message
  end
end
