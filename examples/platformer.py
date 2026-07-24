def setup(engine):
    engine.set_gravity(500)
    engine.rect('player', 50, 50, 30, 30, color='#39a0ed')
    engine.rect('ground', 0, 280, 200, 20, color='#4a6a7f')
    engine.rect('platform1', 250, 200, 150, 15, color='#6a9955')
    engine.rect('platform2', 50, 120, 100, 15, color='#6a9955')
    engine.rect('platform3', 350, 120, 100, 15, color='#6a9955')
    engine.circle('coin1', 300, 100, 10, color='#ffd700')
    engine.circle('coin2', 100, 50, 10, color='#ffd700')

def update(engine, dt):
    if engine.is_key('Left'): engine.move('player', dx=-150*dt, dy=0)
    if engine.is_key('Right'): engine.move('player', dx=150*dt, dy=0)
    if engine.is_key_pressed('space') and engine.is_on_ground('player'):
        engine.set_vel('player', vy=-300)
