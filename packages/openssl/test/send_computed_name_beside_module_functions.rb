# A computed name sent to a value that holds a Filter or nil takes no arm
# for a module's own methods (OpenSSL::Random.random_bytes, Util.half):
# only the module object answers them, and the value never holds it.
require "openssl"

module Util
  def self.half(n) = n / 2
end

class Filter
  def &(other) = "and #{other}"
  def |(other) = "or #{other}"
end

f = [Filter.new, nil].first
puts f.__send__("&".to_sym, "x")
op = [:|, :&].first
puts f.__send__(op, "y")
p Util.half(10)
begin
  f.__send__("half".to_sym, "z")
rescue NoMethodError => e
  puts e.message
end
