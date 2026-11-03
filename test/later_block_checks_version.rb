# A lazy chain's block runs when the chain is forced and an END body at
# exit, which may be before or after a def that follows them: a class
# method they call answers if it is defined by then and raises
# NoMethodError if not, as the run-time version check tells. (Each method
# has a name of its own: a name defined on two classes keeps the static
# answer.)

# lazy chains held and forced after the def
class S4; end
l = [1].lazy.map { S4.z4 }
l2 = [1].each_slice(1).lazy.map { S4.z4 }
l3 = (1..3).lazy.select { |x| x > S4.z4 - 4 }.map { |x| x + S4.z4 }
def S4.z4 = 4
p l.first
p l2.to_a
p l3.first(2)

# held and forced before the def, then after it
class S5; end
m = [1].lazy.map { S5.z5 }
p((begin; m.first; rescue NoMethodError; :nme; end))
p((begin; [2].lazy.select { S5.z5 }.to_a; rescue NoMethodError; :nme; end))
def S5.z5 = 5
p m.first

# forced at once, and literal iterators: before the def, it is missing
class S6; end
p((begin; [1].lazy.map { S6.z6 }.first; rescue NoMethodError; :nme; end))
p((begin; [1].lazy.each { S6.z6 }; rescue NoMethodError; :nme; end))
[1].each { p((begin; S6.z6; rescue NoMethodError; :nme; end)) }
1.tap { p((begin; S6.z6; rescue NoMethodError; :nme; end)) }
def S6.z6 = 6
p [1].lazy.map { S6.z6 }.first

# an END body runs at exit, after the def that follows it
class S7; end
END { p S7.z7 }
def S7.z7 = 7
p :body
