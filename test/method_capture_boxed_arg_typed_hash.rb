# spinel: not-cruby -- a Hash literal typed by its own keys refuses a key of
# another kind stored by a captured method, where CRuby stores it.
#
# method_capture_boxed_arg.rb is the rule: the boxed argument of `call` on a
# boxed receiver reaches the captured method, whose parameter no longer
# holds it to the Hash its direct call passed. A String-keyed Hash of
# another value kind is then stored into as CRuby does. A Hash the program
# wrote with Integer keys keeps its typed form, which a direct call would
# have widened for the store; the store raises TypeError and converts
# nothing.
def put(hash)
  hash["k"] = 1
  hash
end
def run(table, key, value) = table[key].call(value)
p put({"k" => 0})
table = {put: method(:put), id: ->(value) { value }}
values = [{"k" => 5, "j" => 6}, {1 => 2}, "s"]
p run(table, :put, values[0]), run(table, :id, values[2])
begin
  p run(table, :put, values[1])
rescue TypeError
  puts "TypeError"
end
p values[1], put({"z" => 9})
