# A body in a value after one that defines a method_added hook, in the same statement, is placed at the statement as
# any other: the record does not take the statement before the walk has passed its parts. The calls around it and a
# def after it answer as CRuby does. (nested_body_in_value_hook_held is this program with an unrelated held body:
# the answers do not change with it.)
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
