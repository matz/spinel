require "openssl"

first = OpenSSL::ASN1::Integer.new(1)
second = OpenSSL::ASN1::Integer.new(2)
sequence = OpenSSL::ASN1::Sequence.new([first, second])
seen = []
returned = sequence.each do |node|
  seen << node.value
  :ignored
end
p seen
p returned.equal?(sequence)
p sequence.each.equal?(sequence)
p sequence.map { |node| node.value }
nodes = sequence.to_a
p nodes.map { |node| node.value }
p nodes[0].equal?(first)
p nodes[1].equal?(second)
p sequence.each { |node| break :stopped }
empty = OpenSSL::ASN1::Sequence.new([])
p empty.each.equal?(empty)
p empty.to_a

class SyntheticPrivateEach
  attr_reader :calls
  def initialize
    @calls = 0
  end
  private
  def each(&block)
    @calls += 1
    [OpenSSL::ASN1::Integer.new(7)].each(&block)
    :ignored
  end
end
value = SyntheticPrivateEach.new
custom = OpenSSL::ASN1::Sequence.new(value)
p custom.each.equal?(custom)
p value.calls
p custom.map { |node| node.value }
p value.calls
begin
  OpenSSL::ASN1::Sequence.new(nil).each
rescue => error
  puts error.class
end

class SyntheticYieldArguments
  def each
    yield 1, 2
    yield
  end
end
OpenSSL::ASN1::Sequence.new(SyntheticYieldArguments.new).each { |*items| p items }
