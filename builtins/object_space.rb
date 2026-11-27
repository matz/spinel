# ObjectSpace.define_finalizer / undefine_finalizer, spliced by the parser
# when a program (or a library it requires) names them. The rest of
# ObjectSpace (each_object, count_objects) needs a live-object
# index or weak references the collector does not keep, and stays
# unsupported; ObjectSpace::WeakMap is builtins/weak_map.rb, keyed by identity.
#
# The callables live here, in FINALIZERS (with the object's id, the argument
# they are called with), which keeps them alive; the runtime (lib/sp_gc.c)
# watches the objects without keeping them alive and, once one has been
# collected, calls back `run` with the callable's index -- after the
# collection, never inside it. Registrations still standing at exit run then.
module ObjectSpace
  module Finalizers__
    ffi_source <<~C
      #include <stdint.h>
      void sp_fin_set_runner(void (*)(int64_t));
    C
    ffi_callback :runner_fn, [:long], :void
    ffi_func :sp_fin_set_runner, [:runner_fn], :void
    native_func :register,     [:any, :int], :void, "sp_fin_register"
    native_func :unregister_one, [:any], :int,    "sp_fin_unregister_one"

    FINALIZERS = []
    IDS = []
    FREE = []

    def self.release(idx)
      FINALIZERS[idx] = nil
      IDS[idx] = nil
      FREE << idx
    end

    def self.run(idx)
      pr = FINALIZERS[idx]
      id = IDS[idx]
      release(idx)
      begin
        pr.call(id) if pr
      rescue Exception
        # a finalizer's exception is dropped, as CRuby does, and the others
        # still run
      end
      nil
    end
  end

  Finalizers__.sp_fin_set_runner(Finalizers__.method(:run))

  def self.define_finalizer(obj, pr = nil, &blk)
    pr ||= blk
    raise ArgumentError, "wrong type argument #{pr.class} (should be callable)" unless pr.respond_to?(:call)
    if obj.nil? || obj == true || obj == false || obj.is_a?(Integer) || obj.is_a?(Float) || obj.is_a?(Symbol)
      raise ArgumentError, "cannot define finalizer for #{obj.class}"
    end
    idx = Finalizers__::FREE.pop || Finalizers__::FINALIZERS.size
    Finalizers__::FINALIZERS[idx] = pr
    Finalizers__::IDS[idx] = obj.object_id
    Finalizers__.register(obj, idx)
    [0, pr]
  end

  def self.undefine_finalizer(obj)
    while (idx = Finalizers__.unregister_one(obj)) >= 0
      Finalizers__.release(idx)
    end
    obj
  end
end
