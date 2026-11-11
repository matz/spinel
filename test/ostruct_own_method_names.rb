# An OpenStruct answers the methods it has itself -- hash and object_id as
# Integers, nil?, display -- and reads any other name as a member (members
# too, which is not one of its methods). The analysis typed hash and
# object_id as members while their arm answered the Integer, and the C did
# not build; nil? was refused; display read a member on a typed OpenStruct,
# and on a boxed one with ostruct required did not build.
require "ostruct"

o = OpenStruct.new(a: 1)
p o.hash.is_a?(Integer), o.hash == o.hash
p o.object_id.is_a?(Integer)
p o.nil?
v = o.display
puts
p v
p o.members
o.members = [1, 2]
p o.members

w = [o, 1].first.display
puts
p w
