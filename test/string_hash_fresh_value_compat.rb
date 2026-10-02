# Read-only fresh values, rebound parameters, and frozen literals keep compiling.
def fresh_hash_string = +"call"
n = 1
h = {literal: +"fresh", interpolated: "value#{n}", call: fresh_hash_string}
h.each_value { |x| p x }
h.each { |k, x| p k, x }
h.each_pair { |k, x| p x }
h.values.each { |x| p x }
h.each_value { |x| x = +"local"; x << "!"; p x }
p h

frozen_values = {k: "frozen"}
begin
  frozen_values.each_value { |x| x << "!" }
rescue FrozenError => e
  p e.class
end
begin
  frozen_values.each { |k, x| x << "!" }
rescue FrozenError => e
  p e.class
end
begin
  frozen_values.each_pair { |k, x| x << "!" }
rescue FrozenError => e
  p e.class
end
begin
  frozen_values.values.each { |x| x << "!" }
rescue FrozenError => e
  p e.class
end
p frozen_values

# Fresh elements do not distinguish a rebind before an append from one after it.
a = [+"r"]
a.each { |x| t = x; t = +"s"; t << "!"; p t }
p a
