# The NoMethodError a call raises for a method no class defines reports the
# receiver the call was evaluated on. A constant receiver an argument
# reassigns (`C.missing(C = 2)`) is read ahead of the argument, as CRuby
# evaluates the receiver first, whether the write is a plain one or a
# multiple-assignment target. One no argument assigns is read in place.

def show(recv_note)
  yield
rescue NoMethodError => e
  puts "#{recv_note}: receiver #{e.receiver.inspect}, #{e.args.size} args"
end

C1 = 1
show("write") { C1.missing(C1 = 2, 1..2) }
p C1

C2 = [1]
show("lambda and Array.new") { C2.missing(-> { 0 }, (C2 = [2]; Array.new(1))) }
p C2

C5 = 7
show("multiple assignment") { C5.missing(((C5, z = 8, 9); z), 1..2) }
p C5

C7 = 20
show("untouched") { C7.missing(C1 + 1, 1..2) }
show("other constant") { C7.missing((C1 = 5), 1..2) }
p C7, C1

# a qualified write of the receiver's name in an argument: the path write's
# leaf is its target's name
QK = [1]
p QK.take((::QK = [2]; 1))
