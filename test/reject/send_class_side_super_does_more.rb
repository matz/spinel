# spinel: reject-send-super
# `super` from a class's own send that does more than pass its parameters on
# reaches Object#send, which dispatches on the name at run time. CRuby prints
# "send size" and 3; Spinel has no Object#send to put behind a program
# method's super, and refuses it at this line, as it does an instance's.
class Logged
  def self.send(name, *args)
    puts "send #{name}"
    super
  end
  def self.size = 3
end
p Logged.send(:size)
