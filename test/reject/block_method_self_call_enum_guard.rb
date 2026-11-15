# A method with an enum_for guard that calls itself on another object with
# its block: the inference of its value asked itself without end and the
# compiler overran its stack. It is refused, as a method that uses its block
# and calls itself has been.
class Paged
  def initialize(page)
    @page = page
  end

  def each(&block)
    return enum_for(:each) unless block_given?
    block.call(@page)
    return if @page >= 2
    Paged.new(@page + 1).each(&block)
  end
end

Paged.new(1).each { |x| puts x }
