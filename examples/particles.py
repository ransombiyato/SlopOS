import random, time
def setup(engine):
    for i in range(30):
        x = random.randint(0, 480)
        y = random.randint(0, 320)
        size = random.randint(2, 5)
        color = random.choice(['#ffffff', '#ffd700', '#4a6a7f'])
        engine.rect(f'star_{i}', x, y, size, size, color=color)
    def sparkle():
        for i in range(10):
            x = random.randint(0, 480)
            y = random.randint(0, 320)
            color = random.choice(['#ff6f61', '#ffd700', '#39a0ed'])
            name = f'sparkle_{time.time()}_{i}'
            engine.circle(name, x, y, 3, color=color)
            engine.add_timer(0.5, lambda n=name: engine.delete(n))
    engine.add_timer(0.1, sparkle, repeat=True)
def update(engine, dt): pass
