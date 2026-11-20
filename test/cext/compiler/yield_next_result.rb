# frozen_string_literal: true
require_relative 'yield_next_result'

# Passing the constant to native code makes every read retain its handle.
YIELD_WORD = +"word"
YieldNextNative.append(YIELD_WORD)

def boxed_result(flag)
  values = [:marker]
  values << yield(flag)
  values.last
end

[false, true].each do |flag|
  result = boxed_result(flag) do |early|
    next YIELD_WORD if early
    YIELD_WORD
  end
  p [result.equal?(YIELD_WORD), result]
end

# The direct tail has no next carrier, but still answers the same box.
result = boxed_result(true) { YIELD_WORD }
p [result.equal?(YIELD_WORD), result]

[false, true].each do |flag|
  result = boxed_result(flag) do |early|
    next YIELD_WORD if early
    nil
  end
  p [result.nil?, result.equal?(YIELD_WORD)]
end

result = boxed_result(true) do
  begin
    next YIELD_WORD
  ensure
    YieldNextNative.append(YIELD_WORD)
    GC.start
  end
end
p [result.equal?(YIELD_WORD), result]

# A native result destination continues to take the handle directly.
def native_result
  yield
end
result = native_result { next YIELD_WORD }
p [result.equal?(YIELD_WORD), result]

YIELD_BINARY = +"a\0b"
YieldNextNative.append(YIELD_BINARY)
result = boxed_result(true) { next YIELD_BINARY }
p [result.equal?(YIELD_BINARY), result.bytes]
