# a bare call in a class or module body that finds no method where it runs
# raises there, though the program defines one of its name later
module M
  begin
    mm
  rescue NameError => e
    puts e.message
  end
  def self.mm = :mm
  p mm
end
class K
  begin
    top_later
  rescue NameError => e
    puts e.message
  end
end
def top_later = :top
class K
  p top_later
end
