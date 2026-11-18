# A Method keeps the String reader selected at capture.
module Layer
  def size = 7
end
class Peer
  def size = 81
end
def take(value)
  value.method(:size)
end
def take_public(value)
  value.public_method(:size)
end
text = +"abc"
mixed = [text, Peer.new][0]
class String
  prepend Layer if ENV["SPINEL_TEST_METHOD_PREPEND"] == "1"
end
typed = text.method(:size)
typed_public = text.public_method(:size)
boxed = take(mixed)
boxed_public = take_public(mixed)
p [typed.call, typed_public.call, boxed.call, boxed_public.call]
class String
  def size = 19
end
p [typed.call, typed_public.call, boxed.call, boxed_public.call]
p [text.size, take(mixed).call, take_public(mixed).call]
