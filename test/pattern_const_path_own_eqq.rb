# A class method === of the program answers `in Mod::Klass`, as it answers
# `in Klass` and `when Mod::Klass`.
module M
  class K
    def self.===(o) = o == 7
  end
  class J < K; end
end
p(case 7; in M::K then :own; else :no; end)
p(case 8; in M::K then :own; else :no; end)
p [7, 8, M::K.new, "s"].map { |v| case v; in M::K then :k; in String then :s; else :no; end }
p(case 7; in M::J then :sub; else :no; end)

# a failed `=>` runs the class's own === once
module M
  class Loud
    def self.===(o)
      puts "Loud === #{o.inspect}"
      false
    end
  end
end
class Quiet
  def self.===(o)
    puts "Quiet === #{o.inspect}"
    false
  end
end
begin; 3 => M::Loud; rescue NoMatchingPatternError => e; p e.message; end
begin; 3 => Quiet; rescue NoMatchingPatternError => e; p e.message; end
begin; 3 => M::Loud => v; rescue NoMatchingPatternError => e; p e.message; end
