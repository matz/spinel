# spinel: int64
# The boxed Bigint tag shares Integer's core surface, including inherited
# methods. Its absent-name answer must agree with the small Integer tag.
# Run with --int-overflow=promote.
def probe(value)
  p value.respond_to?(:to_ary)
  p value.respond_to?("to_str")
  p value.respond_to?(:zero?)
  p value.respond_to?(:inspect)
end
[2**80, 7, []].each { |value| probe(value) }
