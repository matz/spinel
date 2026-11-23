# spinel: rbs-seed-run
# A signature describes the method the class ends with: the version a later
# definition replaces keeps the type its own body answers.
class VersionedRel
  def initialize(k) = @k = k
  def to_s = "rel(#{@k})"
end

class VersionedBase
  def self.raw = [1, 2]
  def self.all
    raw
  end
end

def show = VersionedBase.all.to_s
puts show

class VersionedBase
  def self.all
    VersionedRel.new(self)
  end
end

puts show

# Each value the replaced version answers is judged, not their join: an
# Array on one path and a String on the other.
class VersionedPick
  def self.of(flag)
    if flag
      [1]
    else
      "s"
    end
  end
end

def pick(flag) = VersionedPick.of(flag).inspect
puts pick(true)
puts pick(false)

class VersionedPick
  def self.of(flag)
    VersionedRel.new(flag)
  end
end

puts VersionedPick.of(true).to_s
