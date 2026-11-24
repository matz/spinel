require "socket"

module Cable
  class Socket
    X = 1

    def close = nil
  end

  def self.own(name) = Socket.const_defined?(name)
end

names = ARGV.empty? ? %i[TCP_NODELAY X] : %i[A B]
p names.map { |c| Socket.const_defined?(c) }
p %i[TCP_NODELAY].all? { |c| Socket.const_defined? c }
p Cable.own(names[1])
