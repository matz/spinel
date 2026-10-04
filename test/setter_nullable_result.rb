# Assignment answers the nullable numeric RHS, even when the writer returns
# a Symbol. Boxing that result must retain nil; ordinary sends answer the body.
class Writer
  # Return a Symbol regardless of the RHS so assignment and direct-call results differ.
  def value=(value)
    :writer_result
  end
end

# Print the boxed value and its nil predicate to expose lost nullable metadata.
def report(value)
  p [value, value.nil?]
end

# Preserve nil from a missing receiver when the assignment returns an Integer RHS.
def assign_integer(writer)
  writer&.value = 7
end

# Preserve nil from a missing receiver when the assignment returns a Float RHS.
def assign_float(writer)
  writer&.value = 1.5
end

writer = Writer.new
report('seed')
report(writer.value = [7][1])
report(writer.value = [1.5][1])
report(writer.value = [7][0])
report(writer.value = [1.5][0])
report(writer.send(:value=, [7][1]))
report(writer.public_send(:value=, [1.5][1]))
report(writer.method(:value=).call([7][1]))

# A nil safe-navigation receiver answers nil even with a nonnil RHS.
# Preserve that fact through stored locals and through helper returns.
writers = [writer]
integer = (writers[1]&.value = 7)
report(integer)
float = (writers[1]&.value = 1.5)
report(float)
report(assign_integer(writers[1]))
report(assign_float(writers[1]))
report(assign_integer(writers[0]))
report(assign_float(writers[0]))
