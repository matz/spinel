# A `loop` whose block raises a class nested in a module: one that is a
# StopIteration ends the loop (its value the exception's result), any other
# leaves it as a raise (#10793: the raised constant is asked of its node,
# whose class a nested leaf alone does not name).
module M
  class Err < StandardError; end
  class Stop < StopIteration; end

  def self.plain
    n = 0
    loop do
      n += 1
      raise Err if n == 3
    end
  rescue Err
    [:caught, n]
  end

  def self.stop
    n = 0
    r = loop do
      n += 1
      raise Stop if n == 4
    end
    [:ended, n, r.class]
  end
end

p M.plain, M.stop
