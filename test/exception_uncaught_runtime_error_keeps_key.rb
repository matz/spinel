# A KeyError the runtime raises (`{}.fetch(:a)`) carries its key and receiver
# also when nothing rescues it and no at_exit hook is armed: a reopened
# #message that reads them in the report gets them, as in CRuby, not nil.
class KeyError
  def message
    "key=#{key.inspect} receiver=#{receiver.inspect}"
  end
end

puts "before"
{ b: 1 }.fetch(:a)
