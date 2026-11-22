# A FrozenError the runtime raises (`"abc".freeze << "d"`) carries its
# receiver also when nothing rescues it and no at_exit hook is armed: a
# reopened #message that reads it in the report gets it, as in CRuby, not nil.
class FrozenError
  def message
    "receiver=#{receiver.inspect}"
  end
end

puts "before"
"abc".freeze << "d"
