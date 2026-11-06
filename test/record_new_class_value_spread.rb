# `new(*r, **k)` on a Struct or Data class value (self in its class
# method) builds the record from both spreads.
T = Struct.new(:a, :b)
class T; def self.mk(*r, **k) = self.new(*r, **k); end
K = Struct.new(:a, :b, keyword_init: true)
class K; def self.mk(*r, **k) = self.new(*r, **k); end
D = Data.define(:a, :b)
class D; def self.mk(*r, **k) = self.new(*r, **k); end
p T.mk(5, 6), T.mk(7), T.mk(a: 1, b: 2)
p K.mk(a: 3, b: 4)
p D.mk(1, 2), D.mk(a: 3, b: 4)
