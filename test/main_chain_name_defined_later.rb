# A name only an include or an extend gives is no method of main before the statement that runs it: a bare call there
# raises NameError, and so does `method(:name)`, which names main's singleton class. After it both find the module's.
r = begin
  shout
rescue NameError
  :ne
end
p r
m = begin
  method(:shout)
rescue NameError => e
  e.message.sub(/0x\h+/, "0x")
end
p m
module H
  def shout = :h
end
include H
p shout
p method(:shout).call
p method(:shout).owner

r2 = begin
  whisper
rescue NameError
  :ne
end
p r2
module E
  def whisper = :e
end
extend E
p whisper
p method(:whisper).owner
