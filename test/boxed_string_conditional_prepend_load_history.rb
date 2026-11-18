class BoxedStringConditionalPrependPeer
  def size = 91
end

module BoxedStringConditionalPrepend
  def size = super + 10
end

value = [+"abc", BoxedStringConditionalPrependPeer.new][0]
p value.size
ENV["SPINEL_TEST_PREPEND"] = "yes"
class String
  if ENV["SPINEL_TEST_PREPEND"]
    prepend BoxedStringConditionalPrepend
  end
end
ENV.delete("SPINEL_TEST_PREPEND")
p value.size
class String
  def size = 8
end
p value.size
