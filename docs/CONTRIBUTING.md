# Welcome!
Hello!! Thank you for visiting our contributions file! If you choose to contribute, then please read the rest of this file to gather the necessary information to help out the project! Please read over our [CODE_OF_CONDUCT.md](/docs/CODE_OF_CONDUCT.md) before contributing, and if you haven't already, visit our [README.md](/docs/README.md) to get an overview of the project! Also be sure to visit the copy of the [MIT License](/docs/LICENSE.md).

### Changes to the API:

If you make any changes to the API that might interfere with existing programs, please ensure that these changes are reflected in the examples section [EXAMPLES](examples)

### Philosophy

#### This is the design philosophy we follow:

ProjectV aims to allow upmost flexibility to the user, allowing them to easily use and modify any part of the software they like. This means comments should be clear, concise, and provide meaningful explanations of the code. Code should be allowed to be used for as many use cases as possible, if a function performs two complex tasks, it becomes two seperate functions, allowing for it to be used in other parts of the code.

### Paradigm

#### This is the programming paradigm we loosely try to stick to:

Designed with functional programming in mind due to its flexibility and the fact that the most complex parts of the code are writen in GLSL which doesn't support objects. Object Oriented features are welcome aslong as it can be proven to be a better decision over a functional approach.

#### Details:
- Seperation of concerns. If you have a struct or class, it should only store data. To add functionality you must create a function that takes this struct or class as input.
- Mutable parameters are allowed for performance reasons. However avoid them if it is not necessary.

### Reporting Issues

If you find a bug or have a feature request, please create an issue on our GitHub repository. When reporting an issue, please include:

- A clear and descriptive title.
- A detailed description of the problem or suggestion.
- Steps to reproduce the issue (if applicable).
- Any relevant logs or screenshots.

### Submitting Pull Requests

To contribute code, please follow these steps:

1. Fork the repository.
2. Make your changes to your fork.
4. Ensure your code follows the coding standards outlined below and the paradigm outlined above and is tested for correct functionality.
5. Push your changes to your forked repository.
6. Create a pull request to the main repository.
7. Positively collaborate and discuss with anyone who comments on your pull request.

### Coding Standards

To maintain consistency in the codebase, please adhere to the following coding standards:

- Use meaningful variable and function names, both in camelCase.
- Use snake_case for any sort of callbacks.
- Write clear and concise comments.
- Use: `// Text here.` for inline comments.
- Use:
```cpp
/**
  * This is a comment for describing a function or struct.
  *
  * @param nameOfParameter is what I take in.
  * @return nameOfOutput is what I return.
  */
```
  add comments above functions or structs to allow for code editors to supply function descriptions and information.

### Code Review Process

All pull requests will be reviewed by a project maintainer. During the review process, you may be asked to make changes to your code. Please be responsive to feedback and make the necessary updates.

### Adding of Data Structures

If you add any data structure to the project that are significant enough that they require more explanation, then please include an explanation in the [data_structures](/docs/data_structures) folder.

### Citations

If your changes include anything that should be cited, please include it in the [SOURCES.md](/docs/SOURCES.md). Use this format
```
### What It's Used For
  - [Name of source](Link to source) - Accesed on day/month/year
```

Thank you for contributing to ProjectV! Your help is greatly appreciated.

### More

For more information on this project, visit our [README.md](/docs/README.md)
