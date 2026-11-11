# A call whose last argument is only a `**` operand reaches Array#sum, which takes no keywords: an empty operand passes nothing, a non-empty one is one positional Hash.
def t(l)
  p [l, yield]
rescue => e
  p [l, e.class, e.message]
end
e = {}
n = {a: 1}
t(:e) { [1, 2].sum(**e) }
t(:n) { [1, 2].sum(**n) }
t(:empty_recv_n) { [].sum(**n) }
t(:init_e) { [1, 2].sum(10, **e) }
t(:float_e) { [1.5, 2.0].sum(**e) }
t(:nil) { [1, 2].sum(**nil) }
t(:lit) { [1, 2].sum(**{}) }
x = [3, 4].sum(**e)
p x + 1, x.class
