# spinel: int64
require "openssl"

def compare(a, b)
  p OpenSSL.fixed_length_secure_compare(a, b)
rescue => e
  puts "#{e.class}: #{e.message}"
end

compare("abc".freeze, "abc".freeze)
compare("abc", "abd")
compare("", "")
compare("a", "ab")
compare("\0\xff".b, "\0\xff".b)
compare("\0\xff".b, "\0\xfe".b)
compare("é", "ab")
[nil, true, false, 42, Object.new, :symbol].each { |x| compare(x, "") }
compare(2**70, "")
compare("", nil)

class CompareInput
  def initialize(text) = @text = text
  private def to_str = @text * 1
end
compare(CompareInput.new("binary\0"), CompareInput.new("binary\0"))

class BadCompareInput
  def to_str = 42
end
compare(BadCompareInput.new, "")
class NilCompareInput
  def to_str = nil
end
compare(NilCompareInput.new, "")

class CompareSubclass < String
  def to_str = "changed"
  def to_s = "changed"
  def bytesize = 0
end
compare(CompareSubclass.new("abc"), "abc")
compare(CompareSubclass.new("a"), "aa")

class MutatingCompareInput
  def initialize(first) = @first = first
  def to_str
    @first.replace("longer")
    "longer" * 1
  end
end
first = "short".dup
compare(first, MutatingCompareInput.new(first))

p OpenSSL.secure_compare("abc".freeze, "abc".freeze)
p OpenSSL.secure_compare("abc", "abd")
p OpenSSL.secure_compare("a", "ab")
p OpenSSL.secure_compare("", "")
p OpenSSL.secure_compare("\0\xff".b, "\0\xff".b)
p OpenSSL.secure_compare(CompareInput.new("abc"), "abc")
begin
  OpenSSL.secure_compare(nil, "")
rescue => e
  puts "#{e.class}: #{e.message}"
end

input = "\0\xff".b * 4096
10.times { raise "comparison" unless OpenSSL.fixed_length_secure_compare(input, input.dup) }
p OpenSSL.send(:fixed_length_secure_compare, "abc", "abc")
p ::OpenSSL.method(:secure_compare).call("abc", "abc")
threads = 4.times.map do
  Thread.new { 10.times { raise "comparison" unless OpenSSL.secure_compare(input, input.dup) }; true }
end
p threads.all? { |thread| thread.value }
