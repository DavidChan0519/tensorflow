# 2016 Graphcore
# carlo@graphcore.ai
# ===============================================
"""Convolutional Network for MNIST classification."""

import tensorflow as tf
import numpy as np

def _get_variable(name, shape, initializer, dtype=tf.float32):

  return tf.get_variable(name,
                         shape=shape,
                         initializer=initializer,
                         dtype=dtype)
def inference(x):

  with tf.variable_scope('scale1', use_resource=True):
    x = conv(x, 7, 2, 64)
    x = tf.nn.relu(x)

  with tf.variable_scope('max_pool', use_resource=True):
    x = max_pool(x, ksize=3, stride=2)

  with tf.variable_scope('scale2-1', use_resource=True):
    x = block(x, 1, 64, 256)

  with tf.variable_scope('scale2-2', use_resource=True):
    x = block(x, 1, 64, 256)

  with tf.variable_scope('scale2-3', use_resource=True):
    x = block(x, 1, 64, 256)



  with tf.variable_scope('scale3-1', use_resource=True):
    x = block(x, 2, 128, 512)

  with tf.variable_scope('scale3-2', use_resource=True):
    x = block(x, 1, 128, 512)

  with tf.variable_scope('scale3-3', use_resource=True):
    x = block(x, 1, 128, 512)

  with tf.variable_scope('scale3-4', use_resource=True):
    x = block(x, 1, 128, 512)



  with tf.variable_scope('scale4-1', use_resource=True):
    x = block(x, 2, 256, 1024)

  with tf.variable_scope('scale4-2', use_resource=True):
    x = block(x, 1, 256, 1024)

  with tf.variable_scope('scale4-3', use_resource=True):
    x = block(x, 1, 256, 1024)

  with tf.variable_scope('scale4-4', use_resource=True):
    x = block(x, 1, 256, 1024)

  with tf.variable_scope('scale4-5', use_resource=True):
    x = block(x, 1, 256, 1024)

  with tf.variable_scope('scale4-6', use_resource=True):
    x = block(x, 1, 256, 1024)



  with tf.variable_scope('scale5-1', use_resource=True):
    x = block(x, 2, 512, 2048)

  with tf.variable_scope('scale5-2', use_resource=True):
    x = block(x, 1, 512, 2048)

  with tf.variable_scope('scale5-3', use_resource=True):
    x = block(x, 1, 512, 2048)

  x = tf.reduce_mean(x, reduction_indices=[1, 2])

  with tf.variable_scope('fc', use_resource=True):
    x = fc(x, 1000)

  return x


def block(x, first_stride, internal_filters, final_filters):
  shape_in = x.get_shape()

  shortcut = x

  with tf.variable_scope('a', use_resource=True):
    x = conv(x, 1, first_stride, internal_filters)
    x = tf.nn.relu(x)

  with tf.variable_scope('b', use_resource=True):
    x = conv(x, 3, 1, internal_filters)
    x = tf.nn.relu(x)

  with tf.variable_scope('c', use_resource=True):
    x = conv(x, 1, 1, final_filters)

  with tf.variable_scope('shortcut', use_resource=True):
    pad = int(x.get_shape()[-1] - shape_in[-1])
    kernel = np.reshape(np.concatenate((np.identity(shape_in[-1], dtype=np.float32),
                                        np.zeros([pad, shape_in[-1]]))),
                        [1, 1, shape_in[-1], final_filters])

    shortcut = tf.nn.conv2d(shortcut,
                            kernel,
                            [1,first_stride,first_stride,1],
                            padding='SAME')

  return tf.nn.relu(x + shortcut)


def fc(x, num_units_out):
  num_units_in = x.get_shape()[1]
  weights_initializer = tf.truncated_normal_initializer(stddev=0.01)

  weights = _get_variable('weights', shape=[num_units_in, num_units_out],
                          initializer=weights_initializer)
  biases = _get_variable('biases', shape=[num_units_out],
                         initializer=tf.constant_initializer(0.0))

  x = tf.nn.xw_plus_b(x, weights, biases)

  return x

def conv(x, ksize, stride, filters_out):

  filters_in = x.get_shape()[-1]
  shape = [ksize, ksize, filters_in, filters_out]
  initializer = tf.truncated_normal_initializer(stddev=0.1)

  weights = _get_variable('weights', shape=shape, initializer=initializer)
  return tf.nn.conv2d(x,
                      weights,
                      [1, stride, stride, 1],
                      padding='SAME')


def max_pool(x, ksize=3, stride=2):
  return tf.nn.max_pool(x,
                        ksize=[1, ksize, ksize, 1],
                        strides=[1, stride, stride, 1],
                        padding='SAME')


#
# Main code
#

# Inputs
x = tf.placeholder(tf.float32, shape=[2, 224, 224, 4])
y_ = tf.placeholder(tf.float32, shape=[2, 1000])

# Inference
logits = inference(x)

# Training
cross_entropy = tf.reduce_mean(tf.nn.softmax_cross_entropy_with_logits(logits=logits, labels=y_))
optimizer = tf.train.GradientDescentOptimizer(0.01)
train_step = optimizer.minimize(cross_entropy)

sess = tf.InteractiveSession()

sess.run(tf.global_variables_initializer())

training_data = np.zeros([2, 224, 224, 4]);
training_labels = np.zeros([2, 1000]);

sess.run(train_step, feed_dict={x: training_data, y_: training_labels})

sess.close()
