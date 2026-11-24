# nested_body_in_value_hook with an unrelated body in a value held (Q's, which defines initialize): whether some body
# is held does not change where the record places the others, so every answer is the same.
class K; def m = 0; end
z = [(class K; def self.method_added(n) = nil; end; 0), K.new.m, (class K; def m = 1; end; 0), K.new.m]
p z
p K.new.m

class J; def m = 0; end
z = [(class J; def self.method_added(n) = nil; end; 0), (class J; def m = 1; end; 0)]
p J.new.m
class J; def m = 2; end
p J.new.m

class L; def m = 0; end
p L.new.m
z = [(class L; def self.singleton_method_added(n) = nil; end; 0), (class L; def m = 1; end; 0)]
p L.new.m

z = [(class Q; def initialize; end; end; 0)]
