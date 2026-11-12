# find and rfind on a typed array whose block parameter can hold nil: a write
# past the end of the array (a[5] = 7 on three elements) leaves nils in it, and
# the analysis gives the parameter's slot room for one. The typed loop read the
# element as a plain Integer or Float into that slot, so the C did not compile,
# for rfind (whose backwards loop is the typed one) and for find with an ifnone.
a = [1, 2, 3]
p a.rfind { |x| a[1] = 9 if x == 3; x == 9 }
b = [1, 2, 3]
p b.rfind { |x| b[1] = 7; x == 9 }

c = [1, 2, 3]
c[5] = 7
p c.rfind { |x| x.nil? }
p c.rfind { |x| x == 2 }
p c.rfind { |x| x == 99 }
p c.find { |x| x.nil? }

d = [1.5, 2.5]
d[4] = 3.5
p d.rfind { |x| x.nil? }
p d.rfind { |x| x == 2.5 }

fp = -> { :none }
e = [1, 2, 3]
e[5] = 7
p e.rfind(fp) { |x| x == 99 }
p e.find(fp) { |x| x == 99 }
p e.find(fp) { |x| x.nil? }
p e.rfind(fp) { |x| x == 2 }

f = [1, 2, 3]
p f.rfind { |x| x == 2 }
p f.rfind { |x| x > 5 }
