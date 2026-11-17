# spinel: share
class Trimmer
  def initialize(text)
    @text = text
  end

  def trim(text = @text)
    text.strip!
  end
end

text = +" ab "
trimmer = Trimmer.new(text)
result = proc { trimmer.trim }.call
p text
p result.equal?(text)
