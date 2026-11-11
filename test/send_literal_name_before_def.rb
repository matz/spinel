# A send of a literal name (send, __send__, public_send, with a Symbol or a
# String, a send of a send) is a call of that name at its point: run before
# the method's def it raises NoMethodError, and after the def it runs it,
# as the plain call does. Each name has one def (a name of
# two defs keeps the table's answer).
class A; end
def via_send(o) = (begin; o.send(:am); rescue NoMethodError => e; "#{e.class}: #{e.message}"; end)
def via_pub(o) = (begin; o.public_send(:am); rescue NoMethodError => e; "#{e.class}: #{e.message}"; end)
def via_und(o) = (begin; o.__send__(:am); rescue NoMethodError => e; "#{e.class}: #{e.message}"; end)
def via_str(o) = (begin; o.send("am"); rescue NoMethodError => e; "#{e.class}: #{e.message}"; end)
def via_nested(o) = (begin; o.send(:public_send, "am"); rescue NoMethodError => e; "#{e.class}: #{e.message}"; end)
p [via_send(A.new), via_pub(A.new), via_und(A.new), via_str(A.new), via_nested(A.new)]
class A; def am = :late; end
p [via_send(A.new), via_pub(A.new), via_und(A.new), via_str(A.new), via_nested(A.new)]

# a class method
class B; end
def cm_all = [(begin; B.send(:cm); rescue NoMethodError => e; "#{e.class}: #{e.message}"; end), (begin; B.public_send(:cm); rescue NoMethodError => e; "#{e.class}: #{e.message}"; end), (begin; B.__send__("cm"); rescue NoMethodError => e; "#{e.class}: #{e.message}"; end)]
p cm_all
def B.cm = :cm
p cm_all
