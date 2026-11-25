class Quiet
  def method_missing(name, *args) = 0
end
module MSU
  module Events
    class Proxy
      def method_missing(name, *args) = 1
    end
  end
  def self.make = Events::Proxy.new
end
