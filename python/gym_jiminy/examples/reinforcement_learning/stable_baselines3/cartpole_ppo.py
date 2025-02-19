"""Solve the official Open AI Gym Acrobot problem simulated in Jiminy using PPO
algorithm of Stable_baseline3 reinforcement learning framework.

It solves it consistently in less than 100000 timesteps in average, and in
about 40000 at best.

.. warning::
    This script requires pytorch>=1.4 and stable-baselines3[extra]==0.9.
"""
# flake8: noqa

# ======================== User parameters =========================

GYM_ENV_NAME = "gym_jiminy.envs:cartpole"
GYM_ENV_KWARGS = {
    'continuous': True
}
SEED = 0
N_THREADS = 8
N_GPU = 1

# =================== Configure Python workspace ===================

# GPU device selection must be done at system level to be taken into account
__import__("os").environ["CUDA_VISIBLE_DEVICES"] = \
    ",".join(map(str, range(N_GPU)))

import gymnasium as gym

from stable_baselines3.ppo import MlpPolicy, PPO
from stable_baselines3.common.vec_env import DummyVecEnv, SubprocVecEnv

from tools.utilities import initialize, train, test

# ================== Initialize Tensorboard daemon =================

log_path = initialize()

# ================== Configure learning algorithm ==================

# PPO config
agent_cfg = {}
agent_cfg['n_steps'] = 128
agent_cfg['batch_size'] = 128
agent_cfg['learning_rate'] = 1.0e-3
agent_cfg['n_epochs'] = 8
agent_cfg['gamma'] = 0.99
agent_cfg['gae_lambda'] = 0.95
agent_cfg['target_kl'] = None
agent_cfg['ent_coef'] = 0.01
agent_cfg['vf_coef'] = 0.5
agent_cfg['clip_range'] = 0.2
agent_cfg['clip_range_vf'] = float('inf')
agent_cfg['max_grad_norm'] = float('inf')
agent_cfg['seed'] = SEED

# ====================== Run the optimization ======================

# Create a multiprocess environment
env_creator = lambda: gym.make(GYM_ENV_NAME, **GYM_ENV_KWARGS)
train_env = SubprocVecEnv([env_creator for _ in range(int(N_THREADS//2))],
                          start_method='fork')
test_env = DummyVecEnv([env_creator])

# Create the learning agent according to the chosen algorithm
train_agent = PPO(MlpPolicy, train_env, **agent_cfg,
    tensorboard_log=log_path, verbose=True)
train_agent.eval_env = test_env

# Run the learning process
checkpoint_path = train(train_agent, max_timesteps=100000)

# ===================== Enjoy the trained agent ======================

# Create testing agent
test_agent = train_agent.load(checkpoint_path)
test_agent.eval_env = test_env

# Run the testing process
test(test_agent, max_episodes=1)
