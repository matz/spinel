class Game
  attr_accessor :score, :title

  def initialize
    @score = 0
    @title = "game"
  end
end

class Seat
  attr_reader :game

  def initialize(game)
    @game = game
  end
end

class VM
  def initialize
    @fns = {}
  end

  def expose(name, &blk)
    @fns[name] = blk
  end

  def call(name, arg) = @fns[name].call(arg)
end

class Round
  def initialize(game)
    @game = game
  end

  def each
    yield 1
    yield 2
  end

  def play = each { |n| @game.score += n }
end

game = Game.new
x = (game.score += 2)
p x
add = proc { game.score += 3 }
p add.call
double = -> { game.score *= 2 }
p double.call
vm = VM.new
vm.expose("add_score") { |n| game.score += n }
p vm.call("add_score", 7)
seat = Seat.new(game)
y = (seat.game.score += 100)
p y
p [1, 2].map { |i| game.score += i }
t = (game.title += "!")
p t
p "#{game.score -= 1}"
3.times { game.score += 1 }
p [game.score, game.title]
p Round.new(game).play
