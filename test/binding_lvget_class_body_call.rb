class Probe
  def self.preload(codes, **options)
    add(keys: options.fetch(:keys, codes), if: options.fetch(:if, nil))
  end

  def self.add(keys:, if: nil)
    @defs = [{ if: binding.local_variable_get(:if), keys: keys }]
    p @defs
  end
end

class Own < Probe
  preload %w[c]
end
