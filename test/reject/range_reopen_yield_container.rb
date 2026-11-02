# A yielding method of a reopened Range (or Float, Symbol, Random, Time,
# NilClass) that hands its block an Array or Hash is refused: such a method
# has no function of its own, and its proc form yields the container boxed,
# which a block typed for the container would read back as garbage. The
# call used to name the missing function and fail in the C compiler.
class Range
  def items
    yield [first, last]
  end
end
(1..3).items { |a| p a }
