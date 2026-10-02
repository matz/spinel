# A narrowed boxed String element cannot lend itself to an appending method.
def go(e) = e << "!"
m = [+"x", 1]; m.each { |el| go(el) if el.is_a?(String) }; p m
