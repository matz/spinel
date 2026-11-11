# send, public_send and __send__ with a literal name, and with a name known only at run time.
def t(l)
  p [l, yield]
rescue => e
  p [l, e.class, e.message]
end
e = {}
n = {a: 1}
t(:send_sum) { [1, 2].send(:sum, **e) }
t(:pub_push_n) { [1].public_send(:push, **n) }
t(:usend_push_e) { [1].__send__(:push, **e) }
t(:send_join_e) { [1, 2].send(:join, "-", **e) }
m = :sum
t(:send_dyn) { [1, 2].send(m, **e) }
