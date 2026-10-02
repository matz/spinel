# A class is an instance of the modules it extends, of the modules those
# include, and of the ones its superclasses extend: they are its singleton's
# ancestors. `K.is_a?(E)` answered false, typed or boxed.
module E; def foo = 1; end
module F; include E; end
module G; end
class K; extend E; end
class K2 < K; end
class L; extend F; end
class N; end
p K.is_a?(E), K2.is_a?(E), L.is_a?(E), L.is_a?(F), N.is_a?(E), K.is_a?(G)
p K.kind_of?(E), K.instance_of?(E), E.is_a?(E)
p [K, N].map { |k| k.is_a?(E) }
p [K, N].map { |k| !k.is_a?(E) }
p [K, N, 3].select { |k| E === k }
p [K].map { |k| k.kind_of?(F) }
