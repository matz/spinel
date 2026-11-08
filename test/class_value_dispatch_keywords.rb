# A class method called with keyword arguments on a Class value known only at
# run time binds them as keywords. The arm count used to take the keyword hash
# for one positional argument and the keyword parameters for positional ones:
# a method with two or more keywords had no arm, so the call was refused, or,
# beside a class whose method takes one argument, raised ArgumentError.

module Claude
  def self.answer(request_id:, input:) = "claude #{request_id} #{input}"
  def self.decide(request_id:, decision:, input:) = "claude #{request_id} #{decision} #{input}"
end

module Codex
end

module Approvals
  def self.answer(scope) = "approvals #{scope}"
end

def answer(protocol, id) = protocol.answer(request_id: id, input: { "q" => 1 })
def decide(protocol, id) = protocol.decide(request_id: id, decision: "allow", input: {})

puts answer(Claude, 1)
puts decide(Claude, 2)

# A class whose method takes one positional argument receives the keywords as
# one Hash, as CRuby passes them.
puts answer(Approvals, 3)

# A class without the method raises NoMethodError.
begin
  answer(Codex, 4)
rescue NoMethodError
  puts "NoMethodError"
end

# A call that leaves out a required keyword raises CRuby's ArgumentError.
def partial(protocol) = protocol.answer(request_id: 5)
begin
  partial(Claude)
rescue ArgumentError => e
  puts "ArgumentError: #{e.message}"
end
puts partial(Approvals)
