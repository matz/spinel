# a[i] &&= v and a[i] ||= v on a String Array answer the String the Array holds:
# the value taken into a bytes slot is that String, as the Hash's form answers.
def same(a, i, r, tag)
  puts "#{tag}: #{a[i].equal?(r)} #{a[i] == r}"
end

x = 5
a = [+"q"]
same(a, 0, (a[0] &&= x.class.to_s), "and class")
a = [+"q"]
same(a, 0, (a[0] &&= "k".encoding.to_s), "and encoding")
a = [+"q"]
same(a, 0, (a[0] &&= "lit"), "and literal")
a = [+"q"]
same(a, 0, (a[0] &&= ""), "and empty")
a = [+"q"]
same(a, 0, (a[0] &&= +"mut"), "and mutable")

e = [+"q", ENV["SPINEL_NO_SUCH_VAR"]]
same(e, 1, (e[1] ||= x.class.to_s), "or class")
e = [+"q", ENV["SPINEL_NO_SUCH_VAR"]]
same(e, 1, (e[1] ||= "k".encoding.to_s), "or encoding")
e = [+"q", ENV["SPINEL_NO_SUCH_VAR"]]
same(e, 1, (e[1] ||= +"mut"), "or mutable")
e = [+"q", ENV["SPINEL_NO_SUCH_VAR"]]
same(e, 0, (e[0] ||= x.class.to_s), "or kept")

# a change through the answer reaches the Array
m = [+"old", nil]
r = (m[0] &&= +"new")
r << "!"
t = (m[1] ||= +"fresh")
t << "?"
p m
