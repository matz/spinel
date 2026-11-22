# A top-level `class File` reopening leaves File::Constants::X and IO::X
# reading the builtin File's constants.
class File
  def self.probe = 1
end
p IO::RDONLY
p IO::LOCK_EX
p File::Constants::LOCK_EX
p File::Constants::NULL
p File.probe
