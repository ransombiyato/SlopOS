import random, time
def setup(engine):
    engine.rect('player', 50, 50, 30, 30, color='#39a0ed')
    engine.set_gravity(500)
    def spawn_enemy():
        x = random.randint(50, 430)
        engine.rect(f'enemy_{random.random()}', x, 0, 25, 25, color='#ff6f61', tag='enemy')
        engine.set_vel(f'enemy', vy=100)
        engine.add_timer(2.0, spawn_enemy)
    engine.add_timer(1.0, spawn_enemy, repeat=True)
def update(engine, dt):
    if engine.is_key('Left'): engine.move('player', dx=-150*dt, dy=0)
    if engine.is_key('Right'): engine.move('player', dx=150*dt, dy=0)
    if engine.is_key_pressed('space'):
        player = engine.get('player')
        bullet = engine.rect(f'bullet_{time.time()}', player.x + player.w, player.y + player.h//2, 10, 5, color='#ffd700')
        engine.set_vel(bullet, vx=300)
        engine.add_timer(2.0, lambda n=bullet: engine.delete(n))
    for enemy in [obj for obj in engine.objects.values() if obj.tag == 'enemy']:
        if engine.collides('player', enemy):
            print("Game Over!"); engine.pause()
