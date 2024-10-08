Pruning
======
1. [Introduction](#introduction)
2. [Pruning types](#pruning-types)
3. [Usage](#usage)

## Introduction
Pruning is the process of removing redundant parameters of a network. The idea bears similarity to the ["optimal brain damage"](http://yann.lecun.com/exdb/publis/pdf/lecun-90b.pdf) hypothesis by Yann LeCun. There are two types of pruning: Unstructured and Structured. Unstructured pruning means finding and removing the less salient connection in the model, the place could be anywhere in the matrix. Structured pruning means deleting entire blocks, filters, or channels.

## Usage
### Script:
```python

from intel_extension_for_transformers.transformers import metrics
from neural_compressor.config import WeightPruningConfig
from intel_extension_for_transformers.transformers.trainer import NLPTrainer
# Replace transformers.Trainer with NLPTrainer
# trainer = transformers.Trainer(......)
trainer = NLPTrainer(......)
metric = metrics.Metric(name="eval_accuracy")
trainer.metrics = tune_metric
pruning_conf = WeightPruningConfig([{"start_step": 0, "end_step": 2}],
                                    target_sparsity=0,9,
                                    pruning_scope="local",
                                    pruning_type="magnitude")
model = trainer.prune(pruning_config=pruning_conf)
```
Please refer to [example](../examples/huggingface/pytorch/text-classification/pruning) for the details.

### Create an instance of Metric
The Metric defines which metric will be used to measure the performance of tuned models.
- example:
    ```python
    metric = metrics.Metric(name="eval_accuracy")
    ```

    Please refer to [metrics document](metrics.md) for the details.

### Create an instance of WeightPruningConfig
[WeightPruningConfig](neural-compressor_neural_compressor_config.py at master · intel_neural-compressor.html) defines which pruning algorithm to use and how to apply it during the training process. Intel® Extension for Transformers supports pruning types "magnitude", "pattern_lock", and "GroupLasso". You can create different pruners for different layers.

- arguments:
    |Argument   |Type       |Description                                        |Default value    |
    |:----------|:----------|:-----------------------------------------------|:----------------|
    |pruning_configs |list of dicts|Which epochs to pruning                     |[{}]         |
    |target_sparsity |float |Initial sparsity goal                     |0.90            |
    |update_frequency|integer|Frequency to updating sparsity                 |1                |
    |pruning_type |string|Pruning algorithm |'snip_momentum' |
   

The WeightPruningConfig contains all the information related to the model pruning behavior. If you have created Metric and WeightPruningConfig instance, then you can create an instance of WeightPruningConfig. Metric and pruner are optional.

- example:
    ```python
    from neural_compressor.config import WeightPruningConfig
    pruning_conf = WeightPruningConfig([{"start_step": 0, "end_step": 2}],
                                        target_sparsity=0,9,
                                        pruning_scope="local",
                                        pruning_type="magnitude")
    ```

### Prune with Trainer
- Prune with Trainer
    NLPTrainer inherits from `transformers.Trainer`, so you can create a trainer like what you do in transformers examples. Then you can prune model with `trainer.prune` function.
    ```python
    model = trainer.prune(pruning_config=pruning_conf)
    ```
