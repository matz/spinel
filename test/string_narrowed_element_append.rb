# A narrowed boxed element hands on its original rooted box, not a new
# String made from copied bytes. The Integer beside it remains unchanged.
def go(e) = e << "!"
m = [+"x", 1]; m.each { |el| go(el) if el.is_a?(String) }; p m
